package main

import (
	"context"
	"log/slog"
	"net"
)

// Provider 是对所有 ASR 服务商的抽象接口
// 无论底层是 WebSocket、gRPC 还是 HTTP，只要实现了这几个方法，就能接入网关
type Provider interface {
	Init(id string, sess *Session) error   // 握手初始化
	SendAudio(pcm []byte) error            // 发送二进制音频
	Finalize() error                       // 发送结束信号
	WatchResults(addr *net.UDPAddr, conn *net.UDPConn) // 监听并回传结果
	Close() error                          // 物理关闭连接
}

// Session 代表一个通话过程，它持有一个具体的 Provider
type Session struct {
	ID        string
	audioChan chan []byte
	ctx       context.Context
	cancel    context.CancelFunc
	closed    int32
	provider  Provider
	Log       *slog.Logger // 结构化日志，自动携带 sid 标签
}

// NewSession 创建会话，并注入具体的 Provider（这就是依赖注入的抽象体现）
func NewSession(id string, p Provider) *Session {
	ctx, cancel := context.WithCancel(context.Background())
	return &Session{
		ID:        id,
		audioChan: make(chan []byte, 4096),
		ctx:       ctx,
		cancel:    cancel,
		provider:  p,
		Log:       slog.With("sid", id), // 核心抽象：日志与 SID 绑定
	}
}