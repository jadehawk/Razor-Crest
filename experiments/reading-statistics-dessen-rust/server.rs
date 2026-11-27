// Compact account-wide server digest, read through a scoped device token.
// The cache belongs to the Dessen app, independent of legacy SD plugins.
const STATS_CACHE_FILE: &[u8] = b"/server-overview.json";
// A failed request leaves the last account snapshot available.
#[inline(never)]
fn json_u32(json: &[u8], name: &[u8]) -> Option<u32> {
    let n = session_num(json, name)?;
    if n < 0 || n > u32::MAX as i64 { None } else { Some(n as u32) }
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
    fn save_cache(&self, output: &mut [u8]) {

        let mut writer=HistoryJson::new(output);
        writer.raw(b"{\"source\":\"server\"");
        let fields: [(&[u8], u32); 11] = [
            (b"total_seconds",self.total_seconds),(b"month_seconds",self.month_seconds),
            (b"month_active_days",self.month_active_days),(b"current_streak",self.current_streak),
            (b"books",self.books),(b"sessions",self.sessions),(b"page_turns",self.page_turns),
            (b"best_day_seconds",self.best_day_seconds),(b"daily_average_seconds",self.daily_average_seconds),
            (b"synced_at",self.last_synced),(b"fetched_at",self.last_synced)];
        for (name,value) in fields {
            writer.field(name,value as u64);
        }
        writer.raw(b"}");
        let Some(used)=writer.finish() else{return;};
        let mut path=[0u8;192];
        let n=StatsSettings::path(&mut path,STATS_CACHE_FILE);
        if n!=0 {let _=dsn_fs_write(&path[..n],&output[..used]);}
    }

    #[inline(never)]
    fn accept_overview(&mut self, payload: &mut [u8], size: usize) -> bool {
        self.attempted = true;
        // Both the standalone overview and the batch receipt contain the
        // same fixed overview fields. Retain a good cache on invalid input.
        let previous = self.available;
        self.available = false;
        self.parse_overview(&payload[..size]);
        if !self.available { self.available = previous; return false; }
        let now = dsn_time();
        if now >= 946_684_800 { self.last_synced = now; }
        self.save_cache(payload);
        true
    }

    #[inline(never)]
    fn fetch(&mut self, pairing: &Pairing, response: &mut [u8]) -> bool {
        self.attempted = true;
        self.attempts = self.attempts.saturating_add(1);
        if pairing.phase != PairPhase::Paired { return false; }
        if let Ok(n) = pairing.authorized(OVERVIEW_HEAD, response, false) {
            return self.accept_overview(response,n);
        }
        false
    }
}
