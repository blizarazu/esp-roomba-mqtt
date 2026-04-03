//! ArduinoOTA (espota) protocol implementation.
//!
//! Handshake sequence:
//!   1. Host sends UDP invitation → device replies "OK" or "AUTH <nonce>"
//!   2. (Optional) Host authenticates with MD5 challenge-response
//!   3. Device opens TCP connection to host
//!   4. Host streams firmware in 1460-byte chunks; device ACKs each
//!   5. Device sends final "OK" or "ERROR..."

use md5::{Digest, Md5};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream, UdpSocket};
use std::time::{Duration, Instant};

const CHUNK_SIZE: usize = 1460;
const CMD_FLASH: u32 = 0;
const CMD_AUTH: u32 = 200;

const UDP_TIMEOUT:        Duration = Duration::from_secs(10);
const TCP_ACCEPT_TIMEOUT: Duration = Duration::from_secs(10);
const TCP_CHUNK_TIMEOUT:  Duration = Duration::from_secs(10);
const TCP_FINAL_TIMEOUT:  Duration = Duration::from_secs(60);

// ── Error type ────────────────────────────────────────────────────────────────

#[derive(Debug)]
pub enum OtaError {
    Io(std::io::Error),
    NoAnswer,
    BadAnswer(String),
    AuthFailed(String),
    TcpTimeout,
    TransferError(String),
    NoResult,
}

impl std::fmt::Display for OtaError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            OtaError::Io(e)             => write!(f, "IO error: {e}"),
            OtaError::NoAnswer          => write!(f, "No response from device (timeout 10 s)"),
            OtaError::BadAnswer(s)      => write!(f, "Unexpected device response: {s}"),
            OtaError::AuthFailed(s)     => write!(f, "Authentication failed: {s}"),
            OtaError::TcpTimeout        => write!(f, "Device did not connect (timeout 10 s)"),
            OtaError::TransferError(s)  => write!(f, "Transfer error: {s}"),
            OtaError::NoResult          => write!(f, "No final result from device (timeout 60 s)"),
        }
    }
}

impl From<std::io::Error> for OtaError {
    fn from(e: std::io::Error) -> Self {
        OtaError::Io(e)
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

fn md5_hex(data: &[u8]) -> String {
    let mut h = Md5::new();
    h.update(data);
    format!("{:x}", h.finalize())
}

// ── Public API ────────────────────────────────────────────────────────────────

/// Flash `firmware` to an ESP8266 at `remote_addr:remote_port` (8266) via ArduinoOTA.
///
/// `firmware_name` is used only in the authentication challenge (MD5 cnonce input).
/// Pass `"firmware.bin"` when streaming embedded bytes without a file path.
///
/// `progress_cb` is called with values in `[0.0, 1.0]` as transfer progresses.
/// `log_cb` is called with human-readable status lines (including trailing `\n`).
pub fn flash(
    remote_addr: &str,
    remote_port: u16,
    password: &str,
    firmware_name: &str,
    firmware: &[u8],
    mut progress_cb: impl FnMut(f32),
    mut log_cb: impl FnMut(String),
) -> Result<(), OtaError> {
    let file_md5 = md5_hex(firmware);
    let content_size = firmware.len();

    // ── Bind TCP listener; let the OS pick an available port ──────────────────
    let listener = TcpListener::bind("0.0.0.0:0")?;
    let local_port = listener.local_addr()?.port();

    // ── Phase 1: UDP invitation ────────────────────────────────────────────────
    let udp = UdpSocket::bind("0.0.0.0:0")?;
    udp.set_read_timeout(Some(UDP_TIMEOUT))?;

    log_cb(format!(
        "Listening on TCP port {local_port}. Sending OTA invitation to {remote_addr}:{remote_port} ({} bytes)...\n",
        content_size
    ));
    let invitation = format!("{CMD_FLASH} {local_port} {content_size} {file_md5}\n");
    udp.send_to(invitation.as_bytes(), (remote_addr, remote_port))?;

    let mut buf = [0u8; 128];
    let (n, _) = udp.recv_from(&mut buf).map_err(|_| OtaError::NoAnswer)?;
    let response = std::str::from_utf8(&buf[..n])
        .unwrap_or("")
        .trim()
        .to_string();

    // ── Phase 2: Authentication (only if device requested it) ─────────────────
    if response != "OK" {
        if let Some(nonce) = response.strip_prefix("AUTH ") {
            let nonce = nonce.trim();

            log_cb("Device requested authentication, sending challenge response...\n".into());
            let cnonce_text = format!("{firmware_name}{content_size}{file_md5}{remote_addr}");
            let cnonce   = md5_hex(cnonce_text.as_bytes());
            let passmd5  = md5_hex(password.as_bytes());
            let result   = md5_hex(format!("{passmd5}:{nonce}:{cnonce}").as_bytes());

            let auth_msg = format!("{CMD_AUTH} {cnonce} {result}\n");
            udp.set_read_timeout(Some(UDP_TIMEOUT))?;
            udp.send_to(auth_msg.as_bytes(), (remote_addr, remote_port))?;

            let (n2, _) = udp
                .recv_from(&mut buf)
                .map_err(|_| OtaError::AuthFailed("No answer to auth challenge".into()))?;
            let auth_resp = std::str::from_utf8(&buf[..n2])
                .unwrap_or("")
                .trim()
                .to_string();

            if auth_resp != "OK" {
                return Err(OtaError::AuthFailed(auth_resp));
            }
            log_cb("Authentication OK.\n".into());
        } else {
            return Err(OtaError::BadAnswer(response));
        }
    } else {
        log_cb("Device accepted (no auth required).\n".into());
    }
    drop(udp);

    // ── Phase 3: Accept TCP connection from device ─────────────────────────────
    // Use non-blocking poll so we can enforce the timeout without extra threads.
    log_cb("Waiting for device to open TCP connection (up to 10 s)...\n".into());
    listener.set_nonblocking(true)?;
    let deadline = Instant::now() + TCP_ACCEPT_TIMEOUT;
    let conn = loop {
        match listener.accept() {
            Ok((stream, _)) => break stream,
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                if Instant::now() >= deadline {
                    return Err(OtaError::TcpTimeout);
                }
                std::thread::sleep(Duration::from_millis(20));
            }
            Err(e) => return Err(OtaError::Io(e)),
        }
    };
    drop(listener);
    conn.set_nonblocking(false)?;

    log_cb(format!(
        "TCP connection established. Uploading {} bytes...\n",
        content_size
    ));

    // ── Phases 4 + 5: Transfer + final ACK ────────────────────────────────────
    transfer(conn, firmware, &mut progress_cb, &mut log_cb)
}

// ── Transfer ──────────────────────────────────────────────────────────────────

fn transfer(
    mut conn: TcpStream,
    firmware: &[u8],
    progress_cb: &mut impl FnMut(f32),
    log_cb: &mut impl FnMut(String),
) -> Result<(), OtaError> {
    let total = firmware.len() as f32;
    let mut sent = 0usize;
    let mut last_logged_pct = 0u32;

    conn.set_write_timeout(Some(TCP_CHUNK_TIMEOUT))?;
    conn.set_read_timeout(Some(TCP_CHUNK_TIMEOUT))?;

    let mut ack_buf = [0u8; 32];

    // Phase 4: stream firmware in 1460-byte chunks
    for chunk in firmware.chunks(CHUNK_SIZE) {
        conn.write_all(chunk)
            .map_err(|e| OtaError::TransferError(e.to_string()))?;

        let n = conn
            .read(&mut ack_buf)
            .map_err(|e| OtaError::TransferError(e.to_string()))?;

        if n == 0 {
            return Err(OtaError::TransferError("Connection closed during transfer".into()));
        }

        sent += chunk.len();
        // Reserve [0.0, 0.95] for the transfer phase; [0.95, 1.0] for final ACK.
        let progress = sent as f32 / total * 0.95;
        progress_cb(progress);

        // Log every 10% milestone
        let pct = (progress * 100.0) as u32;
        let bucket = (pct / 10) * 10;
        if bucket > last_logged_pct {
            last_logged_pct = bucket;
            log_cb(format!("  {bucket}% uploaded ({sent} / {} bytes)\n", firmware.len()));
        }

        // Early exit if device signals an error
        if std::str::from_utf8(&ack_buf[..n]).unwrap_or("").contains('E') {
            return Err(OtaError::TransferError(
                "Device reported error during transfer".into(),
            ));
        }
    }

    // Phase 5: wait for final "OK" or "ERROR..." from device
    log_cb("Transfer complete. Waiting for device to confirm flashing...\n".into());
    conn.set_read_timeout(Some(TCP_FINAL_TIMEOUT))?;
    let mut final_buf = [0u8; 64];
    let mut ok = false;
    let mut err = false;

    while !ok && !err {
        let n = conn.read(&mut final_buf).map_err(|_| OtaError::NoResult)?;
        if n == 0 {
            return Err(OtaError::NoResult);
        }
        let reply = std::str::from_utf8(&final_buf[..n]).unwrap_or("");
        // Check 'E' before 'O' — "ERROR" contains both letters
        if reply.contains('E') {
            err = true;
        } else if reply.contains('O') {
            ok = true;
        }
    }

    if ok {
        progress_cb(1.0);
        Ok(())
    } else {
        Err(OtaError::TransferError("Device reported error".into()))
    }
}
