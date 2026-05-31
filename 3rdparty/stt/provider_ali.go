package main

import (
	"encoding/json"
	"net"
	"time"

	"github.com/gorilla/websocket"
)

type AliProvider struct {
	ws   *websocket.Conn
	sess *Session
}

func (p *AliProvider) Init(id string, sess *Session) error {
	p.sess = sess
	p.sess.Log.Info("连接阿里WebSocket", "url", AliAsrURL)
	ws, _, err := websocket.DefaultDialer.Dial(AliAsrURL, map[string][]string{"Authorization": {"bearer " + AliApiKey}})
	if err != nil { return err }
	p.ws = ws

	p.ws.WriteJSON(map[string]interface{}{
		"header": map[string]interface{}{"action": "run-task", "task_id": id, "streaming": "duplex"},
		"payload": map[string]interface{}{
			"task_group": "audio", "task": "asr", "function": "recognition", "model": "paraformer-realtime-v2",
			"parameters": map[string]interface{}{"format": "pcm", "sample_rate": SampleRate},
		},
	})

	p.ws.SetReadDeadline(time.Now().Add(5 * time.Second))
	for {
		_, msg, err := p.ws.ReadMessage()
		if err != nil { return err }
		var res map[string]interface{}
		json.Unmarshal(msg, &res)
		if h, _ := res["header"].(map[string]interface{}); h["event"] == "task-started" {
			p.sess.Log.Info("阿里ASR任务就绪")
			break
		}
	}
	p.ws.SetReadDeadline(time.Time{})
	return nil
}

func (p *AliProvider) SendAudio(pcm []byte) error { return p.ws.WriteMessage(websocket.BinaryMessage, pcm) }

func (p *AliProvider) Finalize() error {
	p.sess.Log.Info("发送阿里 finish-task")
	return p.ws.WriteJSON(map[string]interface{}{
		"header": map[string]interface{}{"action": "finish-task", "task_id": p.sess.ID, "streaming": "duplex"},
	})
}

func (p *AliProvider) WatchResults(addr *net.UDPAddr, conn *net.UDPConn) {
	defer func() {
		p.Close()
		sessions.Delete(p.sess.ID)
		p.sess.Log.Info("阿里监听协程退出")
	}()
	for {
		p.ws.SetReadDeadline(time.Now().Add(15 * time.Second))
		_, msg, err := p.ws.ReadMessage()
		if err != nil { return }
		var res map[string]interface{}
		json.Unmarshal(msg, &res)
		h, _ := res["header"].(map[string]interface{})
		if h["event"] == "result-generated" {
			payload, _ := res["payload"].(map[string]interface{})
			output, _ := payload["output"].(map[string]interface{})
			s, _ := output["sentence"].(map[string]interface{})
			if s["end_time"] != nil {
				text, _ := s["text"].(string)
				p.sess.Log.Info(">>> 阿里最终结果", "text", text)
				conn.WriteToUDP(append([]byte(p.sess.ID), []byte(text)...), addr)
				select {
				case <-p.sess.ctx.Done(): return
				default:
				}
			}
		}
	}
}

func (p *AliProvider) Close() error { 
	if p.ws != nil { return p.ws.Close() }
	return nil
}
