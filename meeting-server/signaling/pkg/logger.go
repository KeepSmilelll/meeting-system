package pkg

import (
	"io"
	"log/slog"
	"os"
	"sync"
)

var (
	loggerMu sync.RWMutex
	logger   = slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
)

func Logger() *slog.Logger {
	loggerMu.RLock()
	defer loggerMu.RUnlock()
	return logger
}

func SetLogger(custom *slog.Logger) {
	if custom == nil {
		return
	}
	loggerMu.Lock()
	defer loggerMu.Unlock()
	logger = custom
}

func NewTextLogger(writer io.Writer, level slog.Leveler) *slog.Logger {
	if writer == nil {
		writer = os.Stdout
	}

	opts := &slog.HandlerOptions{}
	if level != nil {
		opts.Level = level
	}

	return slog.New(slog.NewTextHandler(writer, opts))
}
