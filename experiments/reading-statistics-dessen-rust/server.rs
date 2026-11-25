// Compact account-wide server digest, read through a scoped device token.
// The cache belongs to the Dessen app, independent of legacy SD plugins.
const STATS_CACHE_FILE: &[u8] = b"/server-overview.json";
// A failed request leaves the last account snapshot available.
fn json_u32(json: &[u8], name: &[u8]) -> Option<u32> {
    let mut i = 0;
    while i + name.len() + 3 <= json.len() {
        if json[i] == b'"' && json.get(i + 1..i + 1 + name.len()) == Some(name)
            && json.get(i + 1 + name.len()) == Some(&b'"') {
            let mut j = i + name.len() + 2;
            while j < json.len() && matches!(json[j], b' ' | b'\t' | b'\n' | b'\r') { j += 1; }
            if json.get(j) != Some(&b':') { i += 1; continue; }
            j += 1;
            while j < json.len() && matches!(json[j], b' ' | b'\t' | b'\n' | b'\r') { j += 1; }
            let mut number = 0u32;
            let start = j;
            while j < json.len() && json[j].is_ascii_digit() {
                number = number.checked_mul(10)?.checked_add((json[j] - b'0') as u32)?;
                j += 1;
            }
            if j == start || (j < json.len() && !matches!(json[j], b',' | b'}' | b' ' | b'\r' | b'\n')) {
                return None;
            }
            return Some(number);
        }
        i += 1;
    }
    None
}

struct ServerOverall {
    attempted: bool,
    attempts: u8,
    available: bool,
    last_synced: u32,
    icon_revealed: bool,
    total_seconds: u32,
    month_seconds: u32,
    month_active_days: u32,
    current_streak: u32,
    books: u32,
    sessions: u32,
    page_turns: u32,
    best_day_seconds: u32,
    daily_average_seconds: u32,
}
impl ServerOverall {
    fn new() -> Self {
        Self { attempted: false, attempts: 0, available: false, last_synced: 0, icon_revealed: false, total_seconds: 0, month_seconds: 0,
            month_active_days: 0, current_streak: 0, books: 0, sessions: 0,
            page_turns: 0, best_day_seconds: 0, daily_average_seconds: 0 }
    }
    fn stale(&self, now: u32) -> bool {
        !self.available || (now >= 946_684_800 &&
            (self.last_synced < 946_684_800 || now.saturating_sub(self.last_synced) >= 86_400))
    }
    #[inline(never)]
    fn load_cached(&mut self) {
        self.attempted = true;
        self.available = false;
        let mut response = [0u8; 640];
        let mut path = [0u8; 192];
        let n = dsn_fs_app_dir(&mut path);
        if n <= 0 || n as usize + STATS_CACHE_FILE.len() > path.len() { return; }
        let end = n as usize;
        path[end..end + STATS_CACHE_FILE.len()].copy_from_slice(STATS_CACHE_FILE);
        let size = dsn_fs_read(&path[..end + STATS_CACHE_FILE.len()], &mut response);
        if size > 0 && size as usize <= response.len() { self.parse_overview(&response[..size as usize]); }
    }

    fn parse_overview(&mut self, payload: &[u8]) {
        if json_string(payload, b"source") != Some(b"server".as_slice()) { return; }
        if let (Some(total_seconds), Some(month_seconds), Some(month_active_days), Some(current_streak),
            Some(books), Some(sessions), Some(page_turns),
            Some(best_day_seconds), Some(daily_average_seconds)) = (
            json_u32(payload, b"total_seconds"), json_u32(payload, b"month_seconds"), json_u32(payload, b"month_active_days"),
            json_u32(payload, b"current_streak"), json_u32(payload, b"books"),
            json_u32(payload, b"sessions"), json_u32(payload, b"page_turns"),
            json_u32(payload, b"best_day_seconds"), json_u32(payload, b"daily_average_seconds")
        ) {
            self.last_synced = json_u32(payload, b"fetched_at").or_else(|| json_u32(payload, b"synced_at")).unwrap_or(0);
            self.total_seconds = total_seconds;
            self.month_seconds = month_seconds;
            self.month_active_days = month_active_days;
            self.current_streak = current_streak;
            self.books = books;
            self.sessions = sessions;
            self.page_turns = page_turns;
            self.best_day_seconds = best_day_seconds;
            self.daily_average_seconds = daily_average_seconds;
            self.available = true;
        }
    }

    #[inline(never)]
    fn save_cache(&self) {
        let mut output = [0u8; 640];
        let prefix = b"{\"source\":\"server\"";
        output[..prefix.len()].copy_from_slice(prefix);
        let mut used = prefix.len();
        let fields: [(&[u8], u32); 11] = [
            (b"total_seconds",self.total_seconds),(b"month_seconds",self.month_seconds),
            (b"month_active_days",self.month_active_days),(b"current_streak",self.current_streak),
            (b"books",self.books),(b"sessions",self.sessions),(b"page_turns",self.page_turns),
            (b"best_day_seconds",self.best_day_seconds),(b"daily_average_seconds",self.daily_average_seconds),
            (b"synced_at",self.last_synced),(b"fetched_at",self.last_synced)];
        for (name,value) in fields {
            let digits = S::new().n(value);
            let digits = digits.get();
            if used + name.len() + digits.len() + 5 >= output.len() { return; }
            output[used..used+2].copy_from_slice(b",\""); used += 2;
            output[used..used+name.len()].copy_from_slice(name); used += name.len();
            output[used..used+2].copy_from_slice(b"\":"); used += 2;
            output[used..used+digits.len()].copy_from_slice(digits); used += digits.len();
        }
        output[used] = b'}'; used += 1;
        let mut path = [0u8; 192];
        let base = dsn_fs_app_dir(&mut path);
        if base > 0 && base as usize + STATS_CACHE_FILE.len() <= path.len() {
            let base = base as usize;
            path[base..base+STATS_CACHE_FILE.len()].copy_from_slice(STATS_CACHE_FILE);
            let _ = dsn_fs_write(&path[..base+STATS_CACHE_FILE.len()], &output[..used]);
        }
    }

    #[inline(never)]
    fn fetch(&mut self, pairing: &Pairing) {
        self.attempted = true;
        self.attempts = self.attempts.saturating_add(1);
        if pairing.phase != PairPhase::Paired { return; }
        let mut response = [0u8; 640];
        if let Ok(n) = pairing.authorized(OVERVIEW_HEAD, &mut response, false) {
            let previous = self.available;
            self.available = false;
            self.parse_overview(&response[..n]);
            if !self.available { self.available = previous; return; }
            let now = dsn_time();
            if now >= 946_684_800 { self.last_synced = now; }
            self.save_cache();
        }
    }
}
