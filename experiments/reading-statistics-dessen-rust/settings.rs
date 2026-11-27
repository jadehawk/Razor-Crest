// App-owned preference; never modifies pairing credentials.
struct StatsSettings { auto_fetch: bool, stats_on_sleep: bool, save_failed: bool }
impl StatsSettings {
    #[inline(never)]
    fn path(path: &mut [u8; 192], suffix: &[u8]) -> usize {
        let n = dsn_fs_app_dir(path);
        if n <= 0 || n as usize + suffix.len() > path.len() { return 0; }
        let n = n as usize;
        path[n..n+suffix.len()].copy_from_slice(suffix);
        n + suffix.len()
    }
    #[inline(never)]
    fn load() -> Self {
        let mut path = [0u8; 192];
        let n = Self::path(&mut path, b"/stats-settings");
        let mut value = [0u8; 1];
        let auto_fetch = n > 0 && dsn_fs_read(&path[..n], &mut value) == 1 && value[0] == 1;
        let n = Self::path(&mut path, b"/stats-on-sleep");
        let stats_on_sleep = n > 0 && dsn_fs_read(&path[..n], &mut value) == 1 && value[0] == 1;
        Self { auto_fetch, stats_on_sleep, save_failed: false }
    }
    #[inline(never)]
    fn toggle_flag(&mut self, sleep: bool) {
        let (value, suffix) = if sleep {
            (&mut self.stats_on_sleep, b"/stats-on-sleep".as_slice())
        } else {
            (&mut self.auto_fetch, b"/stats-settings".as_slice())
        };
        let mut path = [0u8; 192];
        let n = Self::path(&mut path, suffix);
        let next = !*value;
        self.save_failed = n == 0 || dsn_fs_write(&path[..n], &[u8::from(next)]) != 0;
        if !self.save_failed { *value = next; }
    }
}
// IDs are stable for touch hit-testing, but traversal follows screen order.
fn settings_step(focus:u8, forward:bool, sleep:bool, upload:bool)->u8 {
    let order = [0,1,3,2];
    let mut next=order.iter().position(|&row| row==focus).unwrap_or(0);
    for _ in 0..4 {
        next=(next+if forward{1}else{3})%4;
        let row=order[next];
        if (row!=1 || sleep) && (row!=3 || upload) {return row;}
    }
    0
}
fn settings_default(paired:bool, upload_supported:bool)->u8 {
    if paired && upload_supported {3} else {0}
}
impl Ui {
    fn settings_bounds(&self, row: u8) -> (i32, i32, i32, i32) {
        let top = 13 + self.ls + self.lt;
        let h = (self.lb + 24).max(42);
        let y = match row {
            0 => top + self.lt + 20,
            1 => top + self.lt + 28 + h,
            3 => self.content_bottom() - 2*h - 40,
            _ => self.content_bottom() - h - 28,
        };
        (G + 12, y, self.w - 2*G - 24, h)
    }
    #[inline(never)]
    fn settings_page(&self, settings: &StatsSettings, server: &ServerOverall, paired: bool, focus: u8, sleep_supported: bool, upload_supported: bool, history_message: &[u8], history_refresh: Option<bool>) {
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
        let mut yy = y+bh+12;
        if sleep_supported {
            let (sx,sy,sw,sh) = self.settings_bounds(1);
            dsn_stroke(sx,sy,sw,sh,BLACK,if !self.touch && focus == 1 {2} else {1},7,15);
            text(sx+8,sy+(sh-self.lb)/2,sw-72,self.lb,b"Stats on Sleep",BODY,LEFT,BOLD);
            text(sx+sw-58,sy+(sh-self.lb)/2,50,self.lb,
                 if settings.stats_on_sleep {b"On"} else {b"Off"},BODY,RIGHT,BOLD);
            yy = sy+sh+12;
        }
        if !sleep_supported {
            text(x,yy,bw,self.lb,b"Refresh when opening the app",SMALL,LEFT,0);
            text(x,yy+self.lb+4,bw,self.lb,b"if last refresh is over 24 hours old.",SMALL,LEFT,0);
            yy += 2*self.lb+16;
        }
        text(x,yy,bw,self.lb,if paired {b"Server: Paired"} else {b"Server: Not paired"},BODY,LEFT,BOLD);
        let now = dsn_time();
        let age = now.saturating_sub(server.last_synced);
        let last = if server.last_synced == 0 { label!(s(b"Last refresh: Never")) }
            else if now < 946_684_800 { label!(s(b"Last refresh: Time unavailable")) }
            else if age < 3600 { label!(s(b"Last refresh: "), n(age/60), s(b" min ago")) }
            else { label!(s(b"Last refresh: "), n(age/3600), s(b" hours ago")) };
        text(x,yy+self.lb+8,bw,self.lb,last.get(),BODY,LEFT,0);
        if settings.save_failed { text(x,yy+2*self.lb+16,bw,self.lb,b"Could not save setting",BODY,LEFT,BOLD); }
        if upload_supported && paired {
            let (hx,hy,hw,hh)=self.settings_bounds(3);
            dsn_stroke(hx,hy,hw,hh,BLACK,if !self.touch && focus==3 {2}else{1},7,15);
            text(hx+6,hy+(hh-self.lb)/2,hw-12,self.lb,b"Upload & refresh",BODY,CENTER,BOLD);
            let message_y=yy+2*self.lb+16+if settings.save_failed {self.lb+4}else{0};
            let message_h=hy-message_y-4;
            if !history_message.is_empty() && message_h>=self.ls {
                let refresh_h=if history_refresh.is_some() && message_h>=2*self.ls+4 {self.ls+4}else{0};
                dsn_text_box(x,message_y,bw,message_h-refresh_h,history_message,SMALL,LEFT,2,0);
                if refresh_h!=0 {
                    text(x,hy-refresh_h-4,bw,self.ls,
                        if history_refresh==Some(true){b"Server statistics refreshed"}else{b"Server refresh failed"},SMALL,LEFT,BOLD);
                }
            }
        }
        let (x,y,bw,bh) = self.settings_bounds(2);
        dsn_stroke(x,y,bw,bh,BLACK,if !self.touch && focus==2 {2} else {1},7,15);
        text(x+6,y+(bh-self.lb)/2,bw-12,self.lb,if paired && upload_supported {b"Refresh only"} else if paired {b"Refresh now"} else {b"Pair reader first"},BODY,CENTER,BOLD);
    }
}
