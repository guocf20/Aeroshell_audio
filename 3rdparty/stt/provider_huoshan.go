package main

import (
	"bytes"
	"compress/gzip"
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"time"

	"github.com/google/uuid"
	"github.com/gorilla/websocket"
)

type HuoshanProvider struct {
	ws   *websocket.Conn
	seq  int32
	sess *Session
}

func (p *HuoshanProvider) Init(id string, sess *Session) error {
	p.sess = sess
	p.seq = 2 // 从 2 开始，1 留给配置包
	headers := http.Header{}
	headers.Add("X-Api-Resource-Id", "volc.bigasr.sauc.duration")
	headers.Add("X-Api-Access-Key", VolcAccessKey)
	headers.Add("X-Api-App-Key", VolcAppKey)
	headers.Add("X-Api-Request-Id", uuid.New().String())

	p.sess.Log.Info("连接火山WebSocket", "url", VolcAsrURL)
	ws, _, err := websocket.DefaultDialer.Dial(VolcAsrURL, headers)
	if err != nil {
		return err
	}
	p.ws = ws

	// 配置包逻辑
	cfg := map[string]interface{}{
		"user": map[string]interface{}{"uid": id},
		"audio": map[string]interface{}{
			"format": "pcm", "codec": "raw", "rate": SampleRate, "bits": 16, "channel": 1,
		},
		"request": map[string]interface{}{
			"model_name":      "bigmodel",
			"show_utterances": true,
			"result_type":     "single",
			"enable_itn":      true,
		},
	}
	cfgBytes, _ := json.Marshal(cfg)

	// 发送第一个包 (Full Client Request): MsgType=1, Seq=1, Flags=1
	if err := p.ws.WriteMessage(websocket.BinaryMessage, p.pack(1, 1, cfgBytes, true, 0x01)); err != nil {
		return err
	}

	p.ws.SetReadDeadline(time.Now().Add(5 * time.Second))
	_, msg, err := p.ws.ReadMessage()
	p.ws.SetReadDeadline(time.Time{})
	if err != nil {
		return err
	}
	p.sess.Log.Info("收到火山初始化回执", "payload", string(p.unpack(msg)))
	return nil
}

func (p *HuoshanProvider) SendAudio(pcm []byte) error {
	if p.ws == nil {
		return io.ErrClosedPipe
	}
	// 音频包: MsgType=2, Flags=1 (音频流持续发送)
	err := p.ws.WriteMessage(websocket.BinaryMessage, p.pack(2, p.seq, pcm, false, 0x01))
	if err == nil {
		p.seq++ // 必须在这里自增，原本在 return 后面执行不到
	}
	return err
}

func (p *HuoshanProvider) Finalize() error {
	// 负 Seq 表示最后一包，Flags=3 表示音频流结束 (Last Leaf)
	p.sess.Log.Info("发送 Finalize", "seq", -p.seq)
	return p.ws.WriteMessage(websocket.BinaryMessage, p.pack(2, -p.seq, []byte{}, false, 0x03))
}

func (p *HuoshanProvider) WatchResults(addr *net.UDPAddr, conn *net.UDPConn) {
	defer func() {
		p.Close()
		sessions.Delete(p.sess.ID)
		p.sess.Log.Info("监听协程退出，Session销毁")
	}()
	for {
		p.ws.SetReadDeadline(time.Now().Add(15 * time.Second))
		mType, msg, err := p.ws.ReadMessage()
		if err != nil {
			p.sess.Log.Error("读取中断", "err", err)
			p.sess.cancel()
			return
		}
		if mType != websocket.BinaryMessage || len(msg) < 12 {
			continue
		}
		
		payload := p.unpack(msg)
		var res map[string]interface{}
		if err := json.Unmarshal(payload, &res); err != nil {
			continue
		}
		
		if code, ok := res["code"].(float64); ok && code != 20000000 {
			p.sess.Log.Error("业务异常", "code", code, "msg", res["message"])
			p.sess.cancel()
			return
		}
		
		if r, ok := res["result"].(map[string]interface{}); ok {
			if utts, ok := r["utterances"].([]interface{}); ok {
				for _, u := range utts {
					utt := u.(map[string]interface{})
					text, _ := utt["text"].(string)
					definite, _ := utt["definite"].(bool)
					if text != "" {
						if definite {
							p.sess.Log.Info(">>> 最终状态", "text", text)
							conn.WriteToUDP(append([]byte(p.sess.ID), []byte(text)...), addr)
							select {
							case <-p.sess.ctx.Done():
								return
							default:
							}
						}
					}
				}
			}
		}
	}
}

func (p *HuoshanProvider) Close() error {
	if p.ws != nil {
		return p.ws.Close()
	}
	return nil
}

// 核心封包逻辑：严格遵循 4 字节 Header + 4 字节 Seq + 4 字节 PayloadSize
func (p *HuoshanProvider) pack(mType byte, seq int32, payload []byte, compress bool, flags byte) []byte {
	buf := new(bytes.Buffer)
	
	// Byte 0: Protocol Version (0001) | Header Size (0001 -> 4字节) -> 0x11
	buf.WriteByte(0x11)
	
	// Byte 1: Message Type (4bit) | Message Type Specific Flags (4bit)
	buf.WriteByte(byte(mType<<4 | flags))
	
	// Byte 2: Message Serialization (JSON=0001 -> 0x10) | Message Compression
	comp := byte(0)
	if compress {
		var b bytes.Buffer
		w := gzip.NewWriter(&b)
		w.Write(payload)
		w.Close()
		payload = b.Bytes()
		comp = 1
	}
	buf.WriteByte(byte(0x10 | comp))
	
	// Byte 3: Reserved (0x00)
	buf.WriteByte(0x00)
	
	// Bytes 4-7: Sequence Number (Big Endian)
	binary.Write(buf, binary.BigEndian, seq)
	
	// Bytes 8-11: Payload Size (Big Endian)
	binary.Write(buf, binary.BigEndian, uint32(len(payload)))
	
	// Payload
	buf.Write(payload)
	
	return buf.Bytes()
}

func (p *HuoshanProvider) unpack(msg []byte) []byte {
	if len(msg) < 12 {
		return nil
	}
	payload := msg[12:]
	// 检查 Byte 2 的低 4 位是否为 1 (Gzip)
	if msg[2]&0x0f == 1 {
		gr, err := gzip.NewReader(bytes.NewReader(payload))
		if err != nil {
			return payload
		}
		data, _ := io.ReadAll(gr)
		gr.Close()
		return data
	}
	return payload
}