package main

import (
	"bytes"
	"compress/gzip"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"os"
	"strings"
	"sync"
	"time"
    "io"

	"github.com/gorilla/websocket"
)

type Config struct {
    ASRListenAddr string `json:"asr_listen_addr"`
    ASRResultAddr string `json:"asr_result_addr"`
    Provider      string `json:"provider"`

    Volc struct {
        AccessKey  string `json:"access_key"`
        ResourceID string `json:"resource_id"`
        WSURL      string `json:"ws_url"`
    } `json:"volc"`

    AudioGateway struct {
        ListenAddr      string `json:"listen_addr"`
        VadMode         string `json:"vad_mode"`
        SileroModelPath string `json:"silero_model_path"`
    } `json:"audio_gateway"`
}


var config Config

const (
    SessionIDLen = 32

    SampleRate = 16000
    Channels   = 1
    Bits       = 16

    PCMFrameMs      = 10
    CloudPacketMs   = 200
    PCMBytesPer10Ms = 160 * 2

    CloudPacketBytes = PCMBytesPer10Ms * (CloudPacketMs / PCMFrameMs)

    SessionTimeout = 90 * time.Second
)


type ASRProvider interface {
	Start() error
	SendPCM(pcm []byte) error
	End() error
	Close()
}

type Session struct {
	ID string

	Provider ASRProvider

	PCMBuffer bytes.Buffer

	LastActive time.Time

	mu sync.Mutex
}

var (
	sessions sync.Map
	resultUDP *net.UDPConn
)


func loadConfig(path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}

	return json.Unmarshal(data, &config)
}

func printUsage() {
	fmt.Println(`Usage:
  asr-server [options]

Options:
  -c string
        config file path, default: config.json
  -h
        show help

Example:
  asr-server -c config.json`)
}

func main() {
	configPath := "config.json"

	for i := 1; i < len(os.Args); i++ {
		switch os.Args[i] {
		case "-h", "--help":
			printUsage()
			return

		case "-c", "--config":
			if i+1 >= len(os.Args) {
				fmt.Println("missing config path after", os.Args[i])
				printUsage()
				return
			}

			configPath = os.Args[i+1]
			i++

		default:
			fmt.Println("unknown option:", os.Args[i])
			printUsage()
			return
		}
	}

	if err := loadConfig(configPath); err != nil {
		panic(err)
	}

	providerName := config.Provider

	log.Println("ASR service started")
	log.Println("config:", configPath)
	log.Println("provider:", providerName)
	log.Println("listen:", config.ASRListenAddr)
	log.Println("result:", config.ASRResultAddr)

	resultAddr, err := net.ResolveUDPAddr(
		"udp",
		config.ASRResultAddr,
	)
	if err != nil {
		panic(err)
	}

	resultUDP, err = net.DialUDP(
		"udp",
		nil,
		resultAddr,
	)
	if err != nil {
		panic(err)
	}

	udpAddr, err := net.ResolveUDPAddr(
		"udp",
		config.ASRListenAddr,
	)
	if err != nil {
		panic(err)
	}

	conn, err := net.ListenUDP(
		"udp",
		udpAddr,
	)
	if err != nil {
		panic(err)
	}

	go cleanerLoop()

	buf := make([]byte, 65535)

	for {
		n, _, err := conn.ReadFromUDP(buf)
		if err != nil {
			continue
		}

		if n < SessionIDLen {
			continue
		}

		sessionID := string(buf[:SessionIDLen])

		payload := make([]byte, n-SessionIDLen)
		copy(payload, buf[SessionIDLen:n])

		handlePacket(
			providerName,
			sessionID,
			payload,
		)
	}
}

func handlePacket(providerName string, sessionID string, payload []byte) {
	cmd := string(payload)

	switch cmd {
	case "start":
		startSession(providerName, sessionID)
		return

	case "end":
		endSession(sessionID)
		return
	}

	s := getOrCreateSession(providerName, sessionID)
	if s == nil {
		return
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	s.LastActive = time.Now()

	// 缓存 PCM，避免 10ms 一包直接打云服务
	s.PCMBuffer.Write(payload)

	for s.PCMBuffer.Len() >= CloudPacketBytes {
		chunk := make([]byte, CloudPacketBytes)
		_, _ = s.PCMBuffer.Read(chunk)

		if err := s.Provider.SendPCM(chunk); err != nil {
			log.Println("send pcm failed:", sessionID, err)
			return
		}
	}
}

func startSession(providerName string, sessionID string) {
	s := getOrCreateSession(providerName, sessionID)
	if s == nil {
		return
	}

	log.Println("session started:", sessionID)
}

func endSession(sessionID string) {
	v, ok := sessions.Load(sessionID)
	if !ok {
		return
	}

	s := v.(*Session)

	s.mu.Lock()
	defer s.mu.Unlock()

	// 把最后不足 200ms 的 PCM 也发出去
	if s.PCMBuffer.Len() > 0 {
		remain := s.PCMBuffer.Bytes()
		_ = s.Provider.SendPCM(remain)
		s.PCMBuffer.Reset()
	}

	_ = s.Provider.End()
	s.Provider.Close()

	sessions.Delete(sessionID)

	log.Println("session ended:", sessionID)
}

func getOrCreateSession(providerName string, sessionID string) *Session {
	v, ok := sessions.Load(sessionID)
	if ok {
		return v.(*Session)
	}

	provider, err := createProvider(providerName, sessionID)
	if err != nil {
		log.Println("create provider failed:", err)
		return nil
	}

	if err := provider.Start(); err != nil {
		log.Println("provider start failed:", err)
		provider.Close()
		return nil
	}

	s := &Session{
		ID:         sessionID,
		Provider:   provider,
		LastActive: time.Now(),
	}

	actual, _ := sessions.LoadOrStore(sessionID, s)
	return actual.(*Session)
}

func cleanerLoop() {
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()

	for range ticker.C {
		now := time.Now()

		sessions.Range(func(key, value any) bool {
			s := value.(*Session)

			s.mu.Lock()
			timeout := now.Sub(s.LastActive) > SessionTimeout
			s.mu.Unlock()

			if timeout {
				log.Println("session timeout:", s.ID)
				endSession(s.ID)
			}

			return true
		})
	}
}

func createProvider(name string, sessionID string) (ASRProvider, error) {
	switch strings.ToLower(name) {
	case "volc":
		return NewVolcASRClient(sessionID), nil

	case "mock":
		return NewMockProvider(sessionID), nil

	case "aliyun":
		return nil, errors.New("aliyun adapter not implemented yet")

	case "tencent":
		return nil, errors.New("tencent adapter not implemented yet")

	default:
		return nil, fmt.Errorf("unknown provider: %s", name)
	}
}

/* =========================
   Mock Provider
   用来本地测试链路
========================= */

type MockProvider struct {
	SessionID string
}

func NewMockProvider(sessionID string) *MockProvider {
	return &MockProvider{SessionID: sessionID}
}

func (m *MockProvider) Start() error {
	log.Println("[mock] start:", m.SessionID)
	return nil
}

func (m *MockProvider) SendPCM(pcm []byte) error {
	log.Println("[mock] pcm:", m.SessionID, len(pcm))
	return nil
}

func (m *MockProvider) End() error {
	sendResultToGateway(m.SessionID, "这是 mock 识别结果")
	return nil
}

func (m *MockProvider) Close() {}

/* =========================
   Volc ASR Provider
========================= */

type VolcASRClient struct {
    SessionID string

    AccessKey  string
    ResourceID string
    WSURL      string

    conn *websocket.Conn

    closeOnce sync.Once
}


/*
func NewVolcASRClient(sessionID string) *VolcASRClient {
	return &VolcASRClient{
		SessionID: sessionID,

		AppID:      getenv("VOLC_APP_ID", ""),
		AccessKey: getenv("VOLC_ACCESS_KEY", ""),
		ResourceID: getenv("VOLC_RESOURCE_ID", ""),

		Cluster:  getenv("VOLC_CLUSTER", "volcengine_streaming_common"),
		Workflow: getenv("VOLC_WORKFLOW", "audio_in,resample,partition,vad,fe,decode"),

		WSURL: getenv(
			"VOLC_WS_URL",
			"wss://openspeech.bytedance.com/api/v3/sauc/bigmodel",
		),
	}
}
*/

func NewVolcASRClient(sessionID string) *VolcASRClient {
    return &VolcASRClient{
        SessionID: sessionID,

        AccessKey: config.Volc.AccessKey,
        ResourceID: config.Volc.ResourceID,

        WSURL: config.Volc.WSURL,
    }
}

func (v *VolcASRClient) Start() error {
	// 新版控制台只需要 API Key + ResourceID
	// 这里仍然复用 v.AccessKey 存放 X-Api-Key，避免大改结构体
	if v.AccessKey == "" || v.ResourceID == "" {
		return errors.New("missing VOLC_ACCESS_KEY / VOLC_RESOURCE_ID")
	}

	header := map[string][]string{
		// 新版鉴权：这里的 v.AccessKey 实际填写火山控制台的 X-Api-Key
		"X-Api-Key": {v.AccessKey},

		// 2.0 小时版：volc.seedasr.sauc.duration
		// 2.0 并发版：volc.seedasr.sauc.concurrent
		"X-Api-Resource-Id": {v.ResourceID},

		// 请求 ID，建议每次随机
		"X-Api-Request-Id": {randomHex(16)},

		// 文档要求固定 -1
		"X-Api-Sequence": {"-1"},
	}

	conn, resp, err := websocket.DefaultDialer.Dial(v.WSURL, header)
	if err != nil {
		if resp != nil {
			body, _ := io.ReadAll(resp.Body)
			return fmt.Errorf(
				"websocket dial failed: status=%d body=%s err=%v",
				resp.StatusCode,
				string(body),
				err,
			)
		}

		return err
	}

	v.conn = conn

	go v.readLoop()

	req := map[string]any{
		"user": map[string]any{
			"uid": v.SessionID,
		},
		"audio": map[string]any{
			"format":   "pcm",
			"codec":    "raw",
			"rate":     SampleRate,
			"bits":     Bits,
			"channel":  Channels,
			"language": "zh-CN",
            "end_window_size": 200,
		},
		"request": map[string]any{
            "model_name":      "bigmodel",
            "enable_itn":      true,
            "enable_punc":     true,
            "enable_ddc":      false,
            "show_utterances": true,
            "result_type":     "full",
        
            // 火山固定参数
            "workflow": "audio_in,resample,partition,vad,fe,decode",
            "cluster":  "volcengine_streaming_common",
        
            "corpus": map[string]any{
                "context": "",
            },
        },
	}

	payload, err := json.Marshal(req)
	if err != nil {
		return err
	}

	packet := makeVolcPacket(
		MsgTypeFullClientRequest,
		MsgFlagNoSeq,
		SerializationJSON,
		CompressionGzip,
		gzipData(payload),
	)

	return v.conn.WriteMessage(websocket.BinaryMessage, packet)
}

func (v *VolcASRClient) SendPCM(pcm []byte) error {
	if v.conn == nil {
		return errors.New("volc websocket not connected")
	}

	packet := makeVolcPacket(
		MsgTypeAudioOnlyRequest,
		MsgFlagNoSeq,
		SerializationNone,
		CompressionNone,
		pcm,
	)

	return v.conn.WriteMessage(websocket.BinaryMessage, packet)
}

func (v *VolcASRClient) End() error {
	if v.conn == nil {
		return nil
	}

	// 结束包：audio only + negative sequence/end flag
	packet := makeVolcPacket(
		MsgTypeAudioOnlyRequest,
		MsgFlagLastNoSeq,
		SerializationNone,
		CompressionNone,
		nil,
	)

	return v.conn.WriteMessage(websocket.BinaryMessage, packet)
}

func (v *VolcASRClient) Close() {
	v.closeOnce.Do(func() {
		if v.conn != nil {
			_ = v.conn.Close()
		}
	})
}

func (v *VolcASRClient) readLoop() {
	var latestText string

	for {
		_, msg, err := v.conn.ReadMessage()
		if err != nil {
			// 连接关闭前，只发送最后一次识别文本，避免客户端收到中间结果
			if latestText != "" {
				sendResultToGateway(v.SessionID, latestText)
			}
			return
		}

		text := parseVolcResponseText(msg)
		if text != "" {
			// 只缓存最新文本，不立即发给客户端
			latestText = text
		}
	}
}

/* =========================
   Volc Binary Protocol
========================= */

const (
	ProtocolVersion = 0x1
	HeaderSize      = 0x1

	SerializationNone = 0x0
	SerializationJSON = 0x1

	CompressionNone = 0x0
	CompressionGzip = 0x1

	MsgTypeFullClientRequest = 0x1
	MsgTypeAudioOnlyRequest  = 0x2
	MsgTypeFullServerResp    = 0x9
	MsgTypeServerAck         = 0xB
	MsgTypeErrorResp         = 0xF

	MsgFlagNoSeq     = 0x0
	MsgFlagLastNoSeq = 0x2
)

func makeVolcPacket(
	msgType byte,
	msgFlag byte,
	serialization byte,
	compression byte,
	payload []byte,
) []byte {
	header := []byte{
		(ProtocolVersion << 4) | HeaderSize,
		(msgType << 4) | msgFlag,
		(serialization << 4) | compression,
		0x00,
	}

	var buf bytes.Buffer
	buf.Write(header)

	var lenBuf [4]byte
	binary.BigEndian.PutUint32(lenBuf[:], uint32(len(payload)))
	buf.Write(lenBuf[:])

	if len(payload) > 0 {
		buf.Write(payload)
	}

	return buf.Bytes()
}

func parseVolcResponseText(msg []byte) string {
	if len(msg) < 4 {
		log.Println("volc response too short")
		return ""
	}

	headerSize := int(msg[0]&0x0F) * 4
	if len(msg) < headerSize {
		log.Println("invalid header size:", headerSize)
		return ""
	}

	msgType := msg[1] >> 4
	flags := msg[1] & 0x0F
	serialization := msg[2] >> 4
	compression := msg[2] & 0x0F

	payload := msg[headerSize:]

	var seq int32 = 0

	// flags & 0x01 表示后面带 sequence
	if flags&0x01 != 0 {
		if len(payload) < 4 {
			log.Println("response missing sequence")
			return ""
		}

		seq = int32(binary.BigEndian.Uint32(payload[:4]))
		payload = payload[4:]
	}

	isLast := flags&0x02 != 0

	if msgType == MsgTypeErrorResp {
		if len(payload) < 8 {
			log.Println("volc error response too short")
			return ""
		}

		code := binary.BigEndian.Uint32(payload[:4])
		size := binary.BigEndian.Uint32(payload[4:8])
		payload = payload[8:]

		if len(payload) < int(size) {
			log.Println("volc error payload size mismatch")
			return ""
		}

		errMsg := string(payload[:size])
		log.Printf("[volc error] code=%d seq=%d last=%v body=%s\n", code, seq, isLast, errMsg)
		return ""
	}

	if msgType != MsgTypeFullServerResp {
		log.Printf("[volc resp] unsupported msgType=%d flags=%d seq=%d\n", msgType, flags, seq)
		return ""
	}

	if len(payload) < 4 {
		log.Println("response missing payload size")
		return ""
	}

	payloadSize := binary.BigEndian.Uint32(payload[:4])
	payload = payload[4:]

	if len(payload) < int(payloadSize) {
		log.Printf("payload size mismatch: need=%d actual=%d\n", payloadSize, len(payload))
		return ""
	}

	payload = payload[:payloadSize]

	if compression == CompressionGzip {
		data, err := gunzipData(payload)
		if err != nil {
			log.Println("volc gunzip failed:", err)
			return ""
		}
		payload = data
	}

	// 打印火山原始 JSON，方便调试
	log.Printf("[volc json] seq=%d last=%v %s\n", seq, isLast, string(payload))

	if serialization != SerializationJSON {
		log.Println("volc response is not json")
		return ""
	}

	var root any
	if err := json.Unmarshal(payload, &root); err != nil {
		log.Println("volc json parse failed:", err, string(payload))
		return ""
	}

	text := extractVolcText(root)
	if text != "" {
		log.Printf("[volc text] seq=%d last=%v text=%s\n", seq, isLast, text)
	}

	return text
}

func extractVolcText(v any) string {
	m, ok := v.(map[string]any)
	if !ok {
		return ""
	}

	result, ok := m["result"].(map[string]any)
	if !ok {
		return ""
	}

	// 优先取 result.text
	if text, ok := result["text"].(string); ok && text != "" {
		return text
	}

	// 再取 utterances[].text
	utterances, ok := result["utterances"].([]any)
	if !ok {
		return ""
	}

	var parts []string

	for _, item := range utterances {
		u, ok := item.(map[string]any)
		if !ok {
			continue
		}

		text, _ := u["text"].(string)
		if text != "" {
			parts = append(parts, text)
		}
	}

	return strings.Join(parts, "")
}

/* =========================
   UDP result back to C++ gateway
========================= */

func sendResultToGateway(sessionID string, text string) {
	if text == "" {
		return
	}

	var buf bytes.Buffer

	sid := []byte(sessionID)

	if len(sid) >= SessionIDLen {
		buf.Write(sid[:SessionIDLen])
	} else {
		buf.Write(sid)
		buf.Write(bytes.Repeat([]byte{0}, SessionIDLen-len(sid)))
	}

	buf.WriteString(text)

	_, err := resultUDP.Write(buf.Bytes())
	if err != nil {
		log.Println("send result failed:", err)
	}
}


func gzipData(data []byte) []byte {
	var buf bytes.Buffer
	w := gzip.NewWriter(&buf)
	_, _ = w.Write(data)
	_ = w.Close()
	return buf.Bytes()
}

func gunzipData(data []byte) ([]byte, error) {
	r, err := gzip.NewReader(bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	defer r.Close()

	var out bytes.Buffer
	_, err = out.ReadFrom(r)
	return out.Bytes(), err
}

func randomHex(n int) string {
	b := make([]byte, n)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}