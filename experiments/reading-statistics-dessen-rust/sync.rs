// Techy-Notes device-code grant. Fixed-size buffers, no credentials in UI or logs.
// The native reader's session uploader remains the authoritative event producer.
const TOKEN_PATH: &[u8] = b"/.crosspoint/reading-statistics-token.json";
const CODE_HEAD: &[u8] = b"POST https://sync.techy-notes.com/api/device/code\nContent-Type: application/json\n";
const TOKEN_HEAD: &[u8] = b"POST https://sync.techy-notes.com/api/device/token\nContent-Type: application/json\n";
const LOGIN_HEAD: &[u8] = b"POST https://sync.techy-notes.com/api/device/login-code\nAuthorization: Bearer ";
const OVERVIEW_HEAD: &[u8] = b"GET https://sync.techy-notes.com/api/device/overview\nAuthorization: Bearer ";

fn json_string<'a>(json: &'a [u8], name: &[u8]) -> Option<&'a [u8]> {
    let mut i = 0;
    while i + name.len() + 3 <= json.len() {
        if json[i] == b'"' && json.get(i+1..i+1+name.len()) == Some(name)
            && json[i+1+name.len()] == b'"' {
            let mut j = i+name.len()+2;
            while j < json.len() && matches!(json[j], b' ' | b'\t' | b'\r' | b'\n') { j += 1; }
            if json.get(j) != Some(&b':') { i += 1; continue; }
            j += 1;
            while j < json.len() && matches!(json[j], b' ' | b'\t' | b'\r' | b'\n') { j += 1; }
            if json.get(j) != Some(&b'"') { return None; }
            let start = j + 1;
            let mut end = start;
            while end < json.len() && json[end] != b'"' {
                if json[end] == b'\\' { return None; } // secrets and codes are plain ASCII
                end += 1;
            }
            return (end < json.len()).then_some(&json[start..end]);
        }
        i += 1;
    }
    None
}

fn valid_token(t: &[u8]) -> bool {
    t.len() == 72 && t.starts_with(b"tnstats_")
        && t[8..].iter().all(|c| c.is_ascii_hexdigit())
}
fn valid_device_code(t: &[u8]) -> bool {
    t.len() == 64 && t.iter().all(|c| c.is_ascii_hexdigit())
}
fn valid_user_code(t: &[u8]) -> bool {
    t.len() == 9 && t[4] == b'-'
        && t.iter().enumerate().all(|(i, c)| i == 4 || c.is_ascii_uppercase() || c.is_ascii_digit())
}

#[derive(Clone, Copy, PartialEq)]
enum PairPhase { Ready, Code, Paired, Expired, NetworkError, ServerError }

struct Pairing {
    phase: PairPhase,
    secret: [u8; 64],
    code: [u8; 9],
    access_token: [u8; 72],
    web_code: [u8; 9],
    web_started: u32,
    web_valid: bool,
    started: u32,
    message: &'static [u8],
    pending: bool,
}
impl Pairing {
    fn new() -> Self {
        let mut result = Self {
            phase: PairPhase::Ready, secret: [0; 64], code: [0; 9],
            access_token: [0; 72], web_code: [0; 9], web_started: 0, web_valid: false,
            started: 0, message: b"Generate a pairing code to begin", pending: false,
        };
        let mut file = [0u8; 192];
        let n = dsn_fs_read(TOKEN_PATH, &mut file);
        if n > 0 && (n as usize) <= file.len() {
            let raw = &file[..n as usize];
            if let Some(token) = json_string(raw, b"token").or_else(|| json_string(raw, b"access_token")) {
                if valid_token(token) {
                    result.phase = PairPhase::Paired;
                    result.access_token.copy_from_slice(token);
                    result.message = b"Open Web Statistics to sign in";
                }
            }
        }
        result
    }

    fn send(head: &[u8], body: &[u8], buffer: &mut [u8], interactive: bool) -> Result<usize, PairPhase> {
        // Interactive pairing and Web PIN requests may open the native Wi-Fi
        // picker; automatic Overview refresh must not interrupt offline reading.
        if interactive && dsn_wifi_connect() < 0 { return Err(PairPhase::NetworkError); }
        let n = dsn_net_request(head, body, buffer);
        if n < 0 { return Err(PairPhase::NetworkError); }
        let n = n as usize;
        if n + 4 > buffer.len() { return Err(PairPhase::ServerError); }
        let status = u16::from_be_bytes([buffer[0], buffer[1]]);
        let header_len = u16::from_be_bytes([buffer[2], buffer[3]]) as usize;
        if 4 + header_len + n > buffer.len() { return Err(PairPhase::ServerError); }
        if status != 200 {
            let payload = &buffer[4 + header_len..4 + header_len + n];
            if status == 400 && payload.windows(b"authorization_pending".len())
                .any(|part| part == b"authorization_pending") {
                return Err(PairPhase::Ready);
            }
            if status == 400 && payload.windows(b"expired_token".len())
                .any(|part| part == b"expired_token") {
                return Err(PairPhase::Expired);
            }
            return Err(PairPhase::ServerError);
        }
        buffer.copy_within(4 + header_len..4 + header_len + n, 0);
        Ok(n)
    }

    fn post(head: &[u8], body: &[u8], buffer: &mut [u8]) -> Result<usize, PairPhase> {
        Self::send(head, body, buffer, true)
    }

    // The scoped reader token is only sent in a request header to the exact
    // production server. No URL, debug output, UI string or QR contains it.
    fn authorized(&self, prefix: &[u8], buffer: &mut [u8], interactive: bool) -> Result<usize, PairPhase> {
        if self.phase != PairPhase::Paired || !valid_token(&self.access_token) {
            return Err(PairPhase::ServerError);
        }
        let mut head = [0u8; 192];
        if prefix.len() + self.access_token.len() + 1 > head.len() { return Err(PairPhase::ServerError); }
        head[..prefix.len()].copy_from_slice(prefix);
        head[prefix.len()..prefix.len() + 72].copy_from_slice(&self.access_token);
        head[prefix.len() + 72] = b'\n';
        Self::send(&head[..prefix.len() + 73], b"", buffer, interactive)
    }

    fn request_web_login(&mut self) {
        let mut response = [0u8; 512];
        self.web_valid = false;
        match self.authorized(LOGIN_HEAD, &mut response, true) {
            Ok(n) => {
                if let Some(code) = json_string(&response[..n], b"user_code") {
                    if valid_user_code(code) {
                        self.web_code.copy_from_slice(code);
                        self.web_started = dsn_time();
                        self.web_valid = true;
                        self.message = b"Scan to open private statistics";
                        return;
                    }
                }
                self.message = b"Invalid web sign-in response";
            }
            Err(PairPhase::NetworkError) => { self.message = b"Offline. Retry Web PIN"; }
            Err(_) => { self.message = b"Web sign-in unavailable. Retry"; }
        }
    }

    fn begin(&mut self) {
        let mut response = [0u8; 768];
        match Self::post(CODE_HEAD, b"", &mut response) {
            Ok(n) => {
                let data = &response[..n];
                if let (Some(device), Some(user)) =
                    (json_string(data, b"device_code"), json_string(data, b"user_code")) {
                    if valid_device_code(device) && valid_user_code(user) {
                        self.secret.copy_from_slice(device);
                        self.code.copy_from_slice(user);
                        self.started = dsn_time();
                        self.phase = PairPhase::Code;
                        self.message = b"Approve code in the web portal";
                        return;
                    }
                }
                self.phase = PairPhase::ServerError;
                self.message = b"Unexpected server response";
            }
            Err(e) => {
                self.phase = e;
                self.message = b"Cannot reach Techy-Notes";
            }
        }
    }

    fn poll(&mut self) {
        if self.phase != PairPhase::Code { return; }
        if dsn_time().wrapping_sub(self.started) >= 600 {
            self.phase = PairPhase::Expired;
            self.message = b"Code expired. Start again";
            self.secret.fill(0);
            return;
        }
        let mut body = [0u8; 82];
        let prefix = b"{\"device_code\":\"";
        body[..prefix.len()].copy_from_slice(prefix);
        body[prefix.len()..prefix.len()+64].copy_from_slice(&self.secret);
        let suffix = b"\"}";
        let end = prefix.len()+64;
        body[end..end+suffix.len()].copy_from_slice(suffix);
        let mut resp = [0u8; 512];
        match Self::post(TOKEN_HEAD, &body[..end+suffix.len()], &mut resp) {
            Ok(n) => {
                if let Some(token) = json_string(&resp[..n], b"access_token") {
                    if valid_token(token) {
                        let mut file = [0u8; 92];
                        let prefix = b"{\"token\":\"";
                        file[..prefix.len()].copy_from_slice(prefix);
                        file[prefix.len()..prefix.len()+token.len()].copy_from_slice(token);
                        let end = prefix.len() + token.len();
                        file[end..end+3].copy_from_slice(b"\"}\n");
                        if dsn_fs_write(TOKEN_PATH, &file[..end+3]) == 0 {
                            self.phase = PairPhase::Paired;
                            self.access_token.copy_from_slice(token);
                            self.message = b"Reader paired successfully";
                            self.secret.fill(0);
                            self.code.fill(0);
                            return;
                        }
                        self.phase = PairPhase::ServerError;
                        self.message = b"Cannot save pairing credential";
                        return;
                    }
                }
                self.phase = PairPhase::ServerError;
                self.message = b"Invalid pairing token";
            }
            Err(PairPhase::Ready) => { self.message = b"Awaiting approval. Check pairing again"; }
            Err(PairPhase::Expired) => {
                self.phase = PairPhase::Expired;
                self.secret.fill(0);
                self.message = b"Code expired. Start again";
            }
            Err(e) => {
                if e == PairPhase::NetworkError {
                    self.message = b"Offline. Retry pairing";
                } else {
                    self.message = b"Server error. Retry pairing";
                }
            }
        }
    }

    // Schedule network work after the host has displayed a progress frame.
    fn schedule(&mut self) {
        if self.pending { return; }
        self.pending = true;
        self.message = match self.phase {
            PairPhase::Code => b"Checking pairing...",
            PairPhase::Paired => b"Generating web sign-in PIN...",
            _ => b"Requesting pairing code...",
        };
    }

    fn activate(&mut self) {
        self.pending = false;
        match self.phase {
            PairPhase::Code => self.poll(),
            PairPhase::Paired => self.request_web_login(), // login only; never overwrite pairing
            _ => self.begin(),
        }
    }
}

impl Ui {
    // Keep rendering and hit testing in agreement on both Picco and X3 layouts.
    fn sync_button_bounds(&self) -> (i32, i32, i32, i32) {
        let width = (self.w - 2 * G - 48).min(280).max(120);
        let height = 42;
        ((self.w - width) / 2, self.content_bottom() - height - 28, width, height)
    }

    fn disconnect_button_bounds(&self) -> (i32, i32, i32, i32) {
        let (x, y, w, h) = self.sync_button_bounds();
        (x, y - h - 12, w, h)
    }

    fn sync_unpair_confirmation(&self) {
        dsn_clear();
        self.header(6);
        let x = G;
        let w = self.w - 2 * G;
        let y = 13 + self.ls + self.lt;
        let h = (self.content_bottom() - y - 13).max(1);
        self.panel(x, y, w, h);
        text(x + 12, y + 30, w - 24, self.lt, b"Disconnect this reader?", TITLE, CENTER, BOLD);
        text(x + 12, y + self.lt + 60, w - 24, self.lb,
             b"Saved pairing and server totals", SMALL, CENTER, 0);
        text(x + 12, y + self.lt + self.lb + 80, w - 24, self.lb,
             b"will be cleared. Device stats stay.", SMALL, CENTER, 0);
        let (yes_x, no_x, by, bw, bh) = self.overall_sync_confirmation_buttons();
        dsn_stroke(yes_x, by, bw, bh, BLACK, 2, 7, 15);
        dsn_stroke(no_x, by, bw, bh, BLACK, 1, 7, 15);
        text(yes_x + 4, by + (bh - self.lb) / 2, bw - 8, self.lb, b"Disconnect", BODY, CENTER, BOLD);
        text(no_x + 4, by + (bh - self.lb) / 2, bw - 8, self.lb, b"Cancel", BODY, CENTER, 0);
    }

    fn sync_page(&self, pairing: &Pairing, native: bool) {
        dsn_clear();
        self.header(6);
        let x = G;
        let w = self.w - 2 * G;
        let y = 13 + self.ls + self.lt;
        let h = (self.content_bottom() - y - 13).max(1);
        self.panel(x, y, w, h);
        if native {
            let tx = x + 12;
            let tw = w - 24;
            text(tx, y + 18, tw, self.lt, b"Techy-Notes Sync", TITLE, CENTER, BOLD);
            if pairing.phase == PairPhase::Paired {
                text(tx, y + self.lt + 38, tw, self.lb, b"READER PAIRED", BODY, CENTER, BOLD);
                text(tx, y + self.lt + self.lb + 59, tw, self.lb, b"Refresh pulls totals from Techy-Notes", SMALL, CENTER, 0);
                text(tx, y + self.lt + self.lb + 83, tw, self.lb, b"Open: Refresh stats", BODY, CENTER, BOLD);
                if !self.touch {
                    text(tx, y + self.lt + self.lb + 111, tw, self.lb,
                         b"Previous: Disconnect reader", BODY, CENTER, 0);
                }
            } else {
                text(tx, y + self.lt + 38, tw, self.lb, b"NOT PAIRED", BODY, CENTER, BOLD);
                text(tx, y + self.lt + self.lb + 59, tw, self.lb, b"Connect using this Dessen app", SMALL, CENTER, 0);
                text(tx, y + self.lt + self.lb + 83, tw, self.lb, b"Open: Pair reader", BODY, CENTER, BOLD);
            }
            if self.touch {
                if pairing.phase == PairPhase::Paired {
                    let (dx, dy, dw, dh) = self.disconnect_button_bounds();
                    dsn_stroke(dx, dy, dw, dh, BLACK, 1, 8, 15);
                    text(dx + 8, dy + (dh - self.lb) / 2, dw - 16, self.lb,
                         b"Disconnect reader", BODY, CENTER, 0);
                }
                let (bx, by, bw, bh) = self.sync_button_bounds();
                dsn_stroke(bx, by, bw, bh, BLACK, 2, 8, 15);
                text(bx + 8, by + (bh - self.lb) / 2, bw - 16, self.lb,
                     if pairing.phase == PairPhase::Paired { b"Refresh Stats" } else { b"Pair Reader" },
                     BODY, CENTER, BOLD);
            }
            return;
        }
        let tx = x + 12;
        let tw = w - 24;
        let mut yy = y + 14;
        text(tx, yy, tw, self.lt, b"Techy-Notes Sync", TITLE, CENTER, BOLD);
        yy += self.lt + 13;
        text(tx, yy, tw, self.lb, if pairing.phase == PairPhase::Paired {
            b"READER PAIRED" } else { b"NOT PAIRED" }, BODY, CENTER, BOLD);
        yy += self.lb + 14;
        text(tx, yy, tw, self.lb, pairing.message, BODY, CENTER, 0);
        yy += self.lb + 15;
        if pairing.phase == PairPhase::Code {
            if self.w >= 640 {
                // Landscape: QR and pairing instructions share the card.
                let qx = x + 18;
                let qy = y + 105;
                if !draw_pairing_qr(&pairing.code, false, qx, qy, 205) {
                    text(tx, qy, tw, self.lb, b"QR unavailable", SMALL, CENTER, 0);
                }
                let rx = x + 243;
                let rw = (w - 260).max(100);
                text(rx, y + 138, rw, self.lt, &pairing.code, TITLE, CENTER, BOLD);
                text(rx, y + 174, rw, self.lb, b"sync.techy-notes.com/link", SMALL, CENTER, 0);
                text(rx, y + 202, rw, self.lb, b"Sign in or continue as guest", SMALL, CENTER, 0);
                if !self.touch {
                    text(rx, y + 244, rw, self.lb, b"Open: Check pairing", BODY, CENTER, BOLD);
                }
            } else {
                // Portrait: keep the link above the QR and touch action below.
                text(tx, yy, tw, self.lt, &pairing.code, TITLE, CENTER, BOLD);
                yy += self.lt + 9;
                text(tx, yy, tw, self.lb, b"sync.techy-notes.com/link", SMALL, CENTER, 0);
                yy += self.lb + 8;
                text(tx, yy, tw, self.lb, b"Sign in or continue as guest", SMALL, CENTER, 0);
                yy += self.lb + 20;
                let _ = draw_pairing_qr(&pairing.code, false, (self.w - 246) / 2, yy, 260);
                if !self.touch {
                    text(tx, yy + 252, tw, self.lb, b"Open: Check pairing", BODY, CENTER, BOLD);
                }
            }
        } else if pairing.phase == PairPhase::Paired {
            let valid = pairing.web_valid && dsn_time().wrapping_sub(pairing.web_started) < 600;
            if valid {
                if self.w >= 640 {
                    let qr_x = x + 18;
                    let qr_y = y + 105;
                    let _ = draw_pairing_qr(&pairing.web_code, true, qr_x, qr_y, 205);
                    let rx = x + 243;
                    let rw = (w - 260).max(100);
                    text(rx, y + 138, rw, self.lt, &pairing.web_code, TITLE, CENTER, BOLD);
                    text(rx, y + 174, rw, self.lb, b"sync.techy-notes.com/signin", SMALL, CENTER, 0);
                    text(rx, y + 206, rw, self.lb, b"Single-use PIN: 10 minutes", SMALL, CENTER, 0);
                    if !self.touch {
                        text(rx, y + 248, rw, self.lb, b"Open: New web PIN", BODY, CENTER, BOLD);
                    }
                } else {
                    text(tx, yy, tw, self.lt, &pairing.web_code, TITLE, CENTER, BOLD);
                    yy += self.lt + 8;
                    text(tx, yy, tw, self.lb, b"Scan to open Web Statistics", SMALL, CENTER, 0);
                    yy += self.lb + 16;
                    let _ = draw_pairing_qr(&pairing.web_code, true, (self.w - 246) / 2, yy, 260);
                    if !self.touch {
                        text(tx, yy + 252, tw, self.lb, b"Open: New web PIN", BODY, CENTER, BOLD);
                    }
                }
            } else {
                text(tx, yy, tw, self.lb, b"Your reader is connected to Techy-Notes.", SMALL, CENTER, 0);
                yy += self.lb + 16;
                text(tx, yy, tw, self.lb, b"Get a one-time PIN to open Web Statistics.", SMALL, CENTER, 0);
                if !self.touch {
                    yy += self.lb + 24;
                    text(tx, yy, tw, self.lb, b"Open: Generate web sign-in PIN", BODY, CENTER, BOLD);
                }
            }
        } else {
            text(tx, yy, tw, self.lb, b"Pair without an existing account", SMALL, CENTER, 0);
            if !self.touch {
                yy += self.lb + 20;
                text(tx, yy, tw, self.lb, b"Open: Generate pairing code", BODY, CENTER, BOLD);
            }
        }
        if self.touch && !pairing.pending {
            let (bx, by, bw, bh) = self.sync_button_bounds();
            dsn_stroke(bx, by, bw, bh, BLACK, 2, 8, 15);
            let label: &[u8] = match pairing.phase {
                PairPhase::Code => b"Check Pairing",
                PairPhase::Paired => if pairing.web_valid && dsn_time().wrapping_sub(pairing.web_started) < 600 {
                    b"New Web PIN"
                } else { b"Open Web Stats" },
                _ => b"Generate Code",
            };
            text(bx + 8, by + (bh - self.lb) / 2, bw - 16, self.lb,
                 label, BODY, CENTER, BOLD);
        }
    }
}
