// App-owned preference; never modifies pairing credentials.
struct StatsSettings { auto_fetch: bool, sleep_page: u8, save_failed: bool }
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
        let n = Self::path(&mut path,b"/sleep-page");
        let sleep_page=if n>0 && dsn_fs_read(&path[..n],&mut value)==1 {value[0].min(6)}else{0};
        Self {auto_fetch,sleep_page,save_failed:false}
    }
    #[inline(never)]
    fn save_sleep(&mut self,page:u8){
        let mut path=[0u8;192];
        let n=Self::path(&mut path,b"/sleep-page");
        self.save_failed=n==0 || dsn_fs_write(&path[..n],&[page])!=0;
        if !self.save_failed{self.sleep_page=page;}
    }
    #[inline(never)]
    fn toggle_flag(&mut self,_sleep:bool) {
        let mut path=[0u8;192];
        let n=Self::path(&mut path,b"/stats-settings");
        self.save_failed=n==0 || dsn_fs_write(&path[..n],&[u8::from(!self.auto_fetch)])!=0;
        if !self.save_failed{self.auto_fetch=!self.auto_fetch;}    }
}
// The same compact row order drives drawing, focus and touch hit testing.
const SETTINGS_ORDER:[u8;7]=[0,1,3,2,4,5,6];
fn settings_visible(row:u8,sleep:bool,upload:bool,paired:bool,manage:bool)->bool {
    match row{1=>sleep,3=>upload&&paired,4|5=>paired,6=>manage,_=>true}
}
fn settings_step(focus:u8,forward:bool,sleep:bool,upload:bool,paired:bool,manage:bool)->u8{
    let mut i=SETTINGS_ORDER.iter().position(|&v|v==focus).unwrap_or(0);
    loop{
        i=(i+if forward{1}else{6})%7;
        let row=SETTINGS_ORDER[i];
        if settings_visible(row,sleep,upload,paired,manage){return row;}
    }
}
fn settings_default(paired:bool,upload:bool)->u8 {
    if !paired{2}else if upload{3}else{0}
}
impl Ui {
    fn settings_bounds(&self,row:u8,sleep:bool,upload:bool,paired:bool,manage:bool)->(i32,i32,i32,i32){
        let top=13+self.ls+self.lt;
        let count=SETTINGS_ORDER.iter().filter(|&&r|settings_visible(r,sleep,upload,paired,manage)).count() as i32;
        let i=SETTINGS_ORDER.iter().take_while(|&&r|r!=row)
            .filter(|&&r|settings_visible(r,sleep,upload,paired,manage)).count() as i32;
        let y=top+3*self.ls+23;
        let h=((self.content_bottom()-y-2*self.ls-20)/count).min(self.lb+24).max(self.ls+5);
        (G+12,y+i*h,self.w-2*G-24,h-4)
    }
    #[inline(never)]
    fn settings_page(&self,settings:&StatsSettings,server:&ServerOverall,paired:bool,focus:u8,sleep:bool,upload:bool,message:&[u8],refresh:Option<bool>,manage:bool){
        dsn_clear();self.header(7);
        let top=13+self.ls+self.lt;let w=self.w-2*G;
        self.panel(G,top,w,self.content_bottom()-top-13);
        text(G+12,top+6,w-24,self.ls,b"Server synchronization",SMALL,LEFT,BOLD);
        text(G+12,top+self.ls+9,w-24,self.ls,if paired{b"Server: Paired"}else{b"Server: Not paired"},SMALL,LEFT,BOLD);
        let now=dsn_time();let age=now.saturating_sub(server.last_synced);
        let last=if server.last_synced==0{label!(s(b"Last refresh: Never"))}
            else if now<946_684_800{label!(s(b"Last refresh: Time unavailable"))}
            else if age<3600{label!(s(b"Last refresh: "),n(age/60),s(b" min ago"))}
            else{label!(s(b"Last refresh: "),n(age/3600),s(b" hours ago"))};
        text(G+12,top+2*self.ls+12,w-24,self.ls,last.get(),SMALL,LEFT,0);
        for row in SETTINGS_ORDER{
            if !settings_visible(row,sleep,upload,paired,manage){continue;}
            let (x,y,bw,bh)=self.settings_bounds(row,sleep,upload,paired,manage);
            dsn_stroke(x,y,bw,bh,BLACK,if !self.touch&&focus==row{2}else{1},7,15);
            let caption:&[u8]=match row{
                0=>b"Auto-fetch server statistics",
                1=>sleep_setting_label(if settings.sleep_page==1&&!paired{2}else{settings.sleep_page}),
                2=>if paired&&upload{b"Refresh only"}else if paired{b"Refresh now"}else{b"Pair Reader"},
                3=>b"Upload & refresh",
                4=>b"Open Web Stats",
                5=>b"Disconnect Reader",
                _=>b"Manage Hidden Books"
            };
            if row<2{
                let on=if row==0{settings.auto_fetch}else{settings.sleep_page!=0};
                                let status=if on{b"On".as_slice()}else{b"Off"};
                let sw=((dsn_measure_text(status,SMALL,BOLD)>>16) as i32)+8;
                dsn_text_box(x+6,y+(bh-self.ls)/2,bw-sw-16,self.ls,caption,SMALL,LEFT,1,BOLD);
                text(x+bw-sw-6,y+(bh-self.ls)/2,sw,self.ls,status,SMALL,RIGHT,BOLD);
            }else{text(x+6,y+(bh-self.ls)/2,bw-12,self.ls,caption,SMALL,CENTER,BOLD);}
        }
        let y=self.content_bottom()-2*self.ls-15;
        if settings.save_failed{
            text(G+12,y,w-24,self.ls,b"Could not save setting / sleep image",SMALL,LEFT,BOLD);
        }else if !message.is_empty(){
            dsn_text_box(G+12,y,w-24,self.ls,message,SMALL,LEFT,1,0);
            if refresh.is_some(){text(G+12,y+self.ls,w-24,self.ls,
                if refresh==Some(true){b"Server statistics refreshed"}else{b"Server refresh failed"},SMALL,LEFT,BOLD);}
        }
    }
}