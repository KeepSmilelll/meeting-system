package server

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"errors"
	"math/big"
	"meeting-server/signaling/config"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func writeSelfSignedCertPair(t *testing.T, dir string) (certPath string, keyPath string) {
	t.Helper()

	privateKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("generate rsa key failed: %v", err)
	}

	serial, err := rand.Int(rand.Reader, big.NewInt(1<<62))
	if err != nil {
		t.Fatalf("generate serial failed: %v", err)
	}

	template := x509.Certificate{
		SerialNumber: serial,
		Subject: pkix.Name{
			CommonName: "127.0.0.1",
		},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(24 * time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		DNSNames:              []string{"localhost"},
	}

	derBytes, err := x509.CreateCertificate(rand.Reader, &template, &template, &privateKey.PublicKey, privateKey)
	if err != nil {
		t.Fatalf("create cert failed: %v", err)
	}

	certPath = filepath.Join(dir, "server.crt")
	keyPath = filepath.Join(dir, "server.key")

	certOut, err := os.Create(certPath)
	if err != nil {
		t.Fatalf("create cert file failed: %v", err)
	}
	defer certOut.Close()
	if err := pem.Encode(certOut, &pem.Block{Type: "CERTIFICATE", Bytes: derBytes}); err != nil {
		t.Fatalf("write cert failed: %v", err)
	}

	keyOut, err := os.Create(keyPath)
	if err != nil {
		t.Fatalf("create key file failed: %v", err)
	}
	defer keyOut.Close()
	if err := pem.Encode(keyOut, &pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(privateKey)}); err != nil {
		t.Fatalf("write key failed: %v", err)
	}

	return certPath, keyPath
}

func TestTCPServerCreateListenerPlainTCP(t *testing.T) {
	cfg := config.Load()
	cfg.ListenAddr = "127.0.0.1:0"
	cfg.TLSEnabled = false

	server := NewTCPServer(cfg, NewSessionManager(), nil)
	listener, err := server.createListener()
	if err != nil {
		t.Fatalf("create plain listener failed: %v", err)
	}
	defer listener.Close()

	if listener.Addr() == nil {
		t.Fatal("expected listener address")
	}
}

func TestTCPServerCreateListenerTLSEnabledWithoutFilesFails(t *testing.T) {
	cfg := config.Load()
	cfg.ListenAddr = "127.0.0.1:0"
	cfg.TLSEnabled = true
	cfg.TLSCertFile = ""
	cfg.TLSKeyFile = ""

	server := NewTCPServer(cfg, NewSessionManager(), nil)
	if _, err := server.createListener(); err == nil {
		t.Fatal("expected tls listener creation to fail without cert/key")
	}
}

func TestTCPServerCreateListenerTLSAcceptsHandshake(t *testing.T) {
	tempDir := t.TempDir()
	certPath, keyPath := writeSelfSignedCertPair(t, tempDir)

	cfg := config.Load()
	cfg.ListenAddr = "127.0.0.1:0"
	cfg.TLSEnabled = true
	cfg.TLSCertFile = certPath
	cfg.TLSKeyFile = keyPath

	server := NewTCPServer(cfg, NewSessionManager(), nil)
	listener, err := server.createListener()
	if err != nil {
		t.Fatalf("create tls listener failed: %v", err)
	}
	defer listener.Close()

	done := make(chan error, 1)
	go func() {
		conn, acceptErr := listener.Accept()
		if acceptErr != nil {
			done <- acceptErr
			return
		}
		tlsConn, ok := conn.(*tls.Conn)
		if !ok {
			_ = conn.Close()
			done <- errors.New("expected tls conn")
			return
		}
		if err := tlsConn.Handshake(); err != nil {
			_ = tlsConn.Close()
			done <- err
			return
		}
		_ = tlsConn.Close()
		done <- nil
	}()

	dialConn, err := tls.Dial("tcp", listener.Addr().String(), &tls.Config{
		InsecureSkipVerify: true,
	})
	if err != nil {
		t.Fatalf("tls dial failed: %v", err)
	}
	_ = dialConn.Close()

	select {
	case acceptErr := <-done:
		if acceptErr != nil {
			t.Fatalf("accept failed: %v", acceptErr)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timeout waiting for tls accept")
	}
}
