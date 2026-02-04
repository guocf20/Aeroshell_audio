package main

import (
	"bytes"
	"flag"
	"io"
	"log/slog"
	"net"
	"os"
	"sync"
	"sync/atomic"

	"gopkg.in/natefinch/lumberjack.v2"
	"gopkg.in/yaml.v3"
)

type Config struct {
	Server struct {
		UDPPort      int    `yaml:"udp_port"`
		ForwardAddr  string `yaml:"forward_addr"`
		DefaultModel string `yaml:"default_model"`
	} `yaml:"server"`
	Log struct {
		Path       string `yaml:"path"`
		Level      string `yaml:"level"`
		MaxSize    int    `yaml:"max_size"`
		MaxBackups int    `yaml:"max_backups"`
		MaxAge     int    `yaml:"max_age"`
		Compress   bool   `yaml:"compress"`
	} `yaml:"log"`
}

var (
	GlobalConfig  Config
	sessions      sync.Map
	AliApiKey     = os.Getenv("ASR_KEY")
	VolcAppKey    = os.Getenv("VOLC_APPKEY")
	VolcAccessKey = os.Getenv("VOLC_ACCESSKEY")
	AliAsrURL     = "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
	VolcAsrURL    = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
	SampleRate    = 16000
)

func NewDefaultConfig() Config {
	c := Config{}
	c.Server.UDPPort = 9000
	c.Server.ForwardAddr = "127.0.0.1:8001"
	c.Server.DefaultModel = "volc"
	c.Log.Level = "info"
	c.Log.MaxSize = 100
	c.Log.MaxBackups = 7
	c.Log.MaxAge = 28
	c.Log.Compress = true
	return c
}

func initLogger(cfg Config) {
	var writers []io.Writer
	writers = append(writers, os.Stdout)
	if cfg.Log.Path != "" {
		writers = append(writers, &lumberjack.Logger{
			Filename:   cfg.Log.Path,
			MaxSize:    cfg.Log.MaxSize,
			MaxBackups: cfg.Log.MaxBackups,
			MaxAge:     cfg.Log.MaxAge,
			Compress:   cfg.Log.Compress,
			LocalTime:  true,
		})
	}
	combinedWriter := io.MultiWriter(writers...)
	var level slog.Level
	switch cfg.Log.Level {
	case "debug": level = slog.LevelDebug
	case "warn":  level = slog.LevelWarn
	case "error": level = slog.LevelError
	default:      level = slog.LevelInfo
	}
	slog.SetDefault(slog.New(slog.NewTextHandler(combinedWriter, &slog.HandlerOptions{Level: level})))
}

func main() {
	configPath := flag.String("c", "config.yaml", "config file path")
	flag.Parse()

	GlobalConfig = NewDefaultConfig()
	if data, err := os.ReadFile(*configPath); err == nil {
		yaml.Unmarshal(data, &GlobalConfig)
	}

	initLogger(GlobalConfig)

	conn, err := net.ListenUDP("udp", &net.UDPAddr{Port: GlobalConfig.Server.UDPPort})
	if err != nil {
		slog.Error("UDP监听失败", "err", err)
		os.Exit(1)
	}
	defer conn.Close()

	fwdAddr, _ := net.ResolveUDPAddr("udp", GlobalConfig.Server.ForwardAddr)
	slog.Info("Go ASR Gateway 启动成功", "port", GlobalConfig.Server.UDPPort, "model", GlobalConfig.Server.DefaultModel, "log_path", GlobalConfig.Log.Path)

	buf := make([]byte, 8192)
	for {
		n, addr, err := conn.ReadFromUDP(buf)
		if err != nil || n < 32 { continue }
		sid := string(buf[:32])
		payload := buf[32:n]
		handlePacket(sid, payload, addr, fwdAddr, conn)
	}
}

func handlePacket(sid string, payload []byte, remote *net.UDPAddr, fwd *net.UDPAddr, conn *net.UDPConn) {
	val, ok := sessions.Load(sid)
	if bytes.Equal(payload, []byte("start")) {
		if !ok {
			var p Provider
			if GlobalConfig.Server.DefaultModel == "volc" {
				p = &HuoshanProvider{}
			} else {
				p = &AliProvider{}
			}
			sess := NewSession(sid, p)
			sessions.Store(sid, sess)
			go runProcessor(sid, sess, fwd, conn)
			sess.Log.Info("Session START", "remote", remote.String())
		}
	} else if bytes.Equal(payload, []byte("end")) {
		if ok {
			sess := val.(*Session)
			atomic.StoreInt32(&sess.closed, 1)
			sess.cancel()
		}
	} else if ok {
		sess := val.(*Session)
		if atomic.LoadInt32(&sess.closed) == 0 {
			data := make([]byte, len(payload))
			copy(data, payload)
			select {
			case sess.audioChan <- data:
			default:
				sess.Log.Warn("缓冲区满，丢弃音频包")
			}
		}
	}
}

func runProcessor(id string, sess *Session, returnAddr *net.UDPAddr, conn *net.UDPConn) {
	if err := sess.provider.Init(id, sess); err != nil {
		sess.Log.Error("Init失败", "err", err)
		sessions.Delete(id)
		return
	}
	go sess.provider.WatchResults(returnAddr, conn)
	for {
		select {
		case pcm, ok := <-sess.audioChan:
			if !ok { return }
			if err := sess.provider.SendAudio(pcm); err != nil {
				sess.Log.Error("发送音频失败", "err", err)
				return
			}
		case <-sess.ctx.Done():
			sess.Log.Info("音频流发送完毕，发送 Finalize")
			sess.provider.Finalize()
			return
		}
	}
}
