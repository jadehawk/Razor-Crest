// App-owned preference; never modifies pairing credentials.
struct StatsSettings { auto_fetch: bool, save_failed: bool }
impl StatsSettings {
    fn path(path: &mut [u8; 192]) -> usize {
        let suffix = b"/stats-settings";
        let n = dsn_fs_app_dir(path);
        if n <= 0 || n as usize + suffix.len() > path.len() { return 0; }
        let n = n as usize;
        path[n..n+suffix.len()].copy_from_slice(suffix);
        n + suffix.len()
    }
    #[inline(never)]
    fn load() -> Self {
        let mut path = [0u8; 192];
        let n = Self::path(&mut path);
        let mut value = [0u8; 1];
        Self { auto_fetch: n > 0 && dsn_fs_read(&path[..n], &mut value) == 1 && value[0] == 1,
               save_failed: false }
    }
    #[inline(never)]
    fn toggle(&mut self) {
        let mut path = [0u8; 192];
        let n = Self::path(&mut path);
        let next = !self.auto_fetch;
        self.save_failed = n == 0 || dsn_fs_write(&path[..n], &[next as u8]) != 0;
        if !self.save_failed { self.auto_fetch = next; }
    }
}
impl Ui {
    fn settings_bounds(&self, row: u8) -> (i32, i32, i32, i32) {
        let top = 13 + self.ls + self.lt;
        let h = (self.lb + 24).max(42);
        let y = if row == 0 { top + self.lt + 20 } else { self.content_bottom() - h - 28 };
        (G + 12, y, self.w - 2*G - 24, h)
    }
    #[inline(never)]
    fn settings_page(&self, settings: &StatsSettings, server: &ServerOverall, paired: bool, focus: u8) {
        dsn_clear();
        self.header(7);
        let top = 13 + self.ls + self.lt;
        let w = self.w - 2*G;
        self.panel(G, top, w, self.content_bottom()-top-13);
        text(G+12, top+8, w-24, self.lt, b"Server synchronization", TITLE, LEFT, BOLD);
        let (x,y,bw,bh) = self.settings_bounds(0);
        dsn_stroke(x,y,bw,bh,BLACK,if !self.touch && focus == 0 {2} else {1},7,15);
        text(x+8,y+(bh-self.lb)/2,bw-72,self.lb,b"Auto-fetch server statistics",BODY,LEFT,BOLD);
        text(x+bw-58,y+(bh-self.lb)/2,50,self.lb,if settings.auto_fetch {b"On"} else {b"Off"},BODY,RIGHT,BOLD);
        let yy = y+bh+12;
        text(x,yy,bw,self.lb,b"Refresh when opening the app",SMALL,LEFT,0);
        text(x,yy+self.lb+4,bw,self.lb,b"if last refresh is over 24 hours old.",SMALL,LEFT,0);
        text(x,yy+2*self.lb+16,bw,self.lb,if paired {b"Server: Paired"} else {b"Server: Not paired"},BODY,LEFT,BOLD);
        let now = dsn_time();
        let age = now.saturating_sub(server.last_synced);
        let last = if server.last_synced == 0 { S::new().s(b"Last refresh: Never") }
            else if now < 946_684_800 { S::new().s(b"Last refresh: Time unavailable") }
            else if age < 3600 { S::new().s(b"Last refresh: ").n(age/60).s(b" min ago") }
            else { S::new().s(b"Last refresh: ").n(age/3600).s(b" hours ago") };
        text(x,yy+3*self.lb+24,bw,self.lb,last.get(),BODY,LEFT,0);
        if settings.save_failed { text(x,yy+4*self.lb+32,bw,self.lb,b"Could not save setting",BODY,LEFT,BOLD); }
        let (x,y,bw,bh) = self.settings_bounds(1);
        dsn_stroke(x,y,bw,bh,BLACK,if !self.touch && focus==1 {2} else {1},7,15);
        text(x+6,y+(bh-self.lb)/2,bw-12,self.lb,if paired {b"Refresh now"} else {b"Pair reader first"},BODY,CENTER,BOLD);
    }
}
