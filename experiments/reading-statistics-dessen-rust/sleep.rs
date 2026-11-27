const SLEEP_ROOT:&[u8]=b"/sleep.bmp";
const SLEEP_BACKUP:&[u8]=b"/sleep-backup.bmp";


// Streaming identity prevents replacing a wallpaper manually changed by the user.
#[inline(never)]
fn sleep_digest(path:&[u8],scratch:&mut [u8])->u64 {
    let mut h=HASH_INIT;
    let mut pos=0u32;
    loop {
        let n=dsn_fs_read_at(path,pos,scratch);
        if n<0 || pos>4*1024*1024{return 0;}
        if n==0{return if pos==0{0}else{h};}
        for &b in &scratch[..n as usize]{h=(h^b as u64).wrapping_mul(1099511628211);}
        pos+=n as u32;
    }
}

#[inline(never)]
fn sleep_publish(enable:bool,account:u64,scratch:&mut [u8])->bool {
    let mut path=[0u8;192];
    let n=StatsSettings::path(&mut path,b"/sleep-owner");
    if n==0{return false;}
    let mut stage=[0u8;192];
    let stage_n=StatsSettings::path(&mut stage,b"/sleep-stage.bmp");
    if stage_n==0{return false;}
    let temp=&stage[..stage_n];
    let mut owner=[0u8;32];
    let loaded=dsn_fs_read(&path[..n],&mut owner);
    if loaded!=32 && loaded!=-2{return false;}
    let old=hist_u64(&owner,0);
    let pending=hist_u64(&owner,8);
    let backup=hist_u64(&owner,16);
    let actual_backup=sleep_digest(SLEEP_BACKUP,scratch);
    let live=sleep_digest(SLEEP_ROOT,scratch);
    let ours=live!=0 && (live==old || live==pending);
    if !enable {
        if ours && dsn_fs_delete(SLEEP_ROOT)!=0{return false;}
        if (ours || live==0) && (old!=0 || pending!=0) &&
            backup!=0 && backup==actual_backup &&
            dsn_fs_rename(SLEEP_BACKUP,SLEEP_ROOT)!=0{return false;}
        if actual_backup!=0 && (backup!=actual_backup || (live!=0&&!ours)){return false;}
        let _=dsn_fs_delete(&path[..n]);
        return true;
    }
    // A manually replaced wallpaper is not ours. Never overwrite an existing backup.
    if actual_backup!=0 && (backup!=actual_backup || (live!=0&&!ours)){return false;}
    let _=dsn_fs_delete(temp);
    if dsn_screenshot(temp)!=0{return false;}
    let next=sleep_digest(temp,scratch);
    if next==0{return false;}
    // Journal both generations before changing the active file.
    owner[..8].copy_from_slice(&if ours{live}else{0}.to_le_bytes());
    owner[8..16].copy_from_slice(&next.to_le_bytes());
    owner[16..24].copy_from_slice(&if live!=0&&!ours{live}else{backup}.to_le_bytes());
    owner[24..].copy_from_slice(&account.to_le_bytes());
    if dsn_fs_write(&path[..n],&owner)!=0{return false;}
    if ours {
        if dsn_fs_delete(SLEEP_ROOT)!=0{return false;}
    }else if live!=0 && dsn_fs_rename(SLEEP_ROOT,SLEEP_BACKUP)!=0{return false;}
    if dsn_fs_rename(temp,SLEEP_ROOT)!=0 {
        if actual_backup!=0 && backup==actual_backup || live!=0&&!ours {
            let _=dsn_fs_rename(SLEEP_BACKUP,SLEEP_ROOT);
        }
        return false;
    }
    true
}

#[inline(never)]
fn sleep_account_changed(account:u64)->bool{
    let mut path=[0u8;192];
    let n=StatsSettings::path(&mut path,b"/sleep-owner");
    let mut owner=[0u8;32];
    n!=0 && dsn_fs_read(&path[..n],&mut owner)==32 && hist_u64(&owner,24)!=account
}

fn sleep_label(p:u8)->&'static [u8]{
    match p {1=>b"Server Statistics",2=>b"Device Statistics",3=>b"Current Book",
        4=>b"Your Week",5=>b"Reading Calendar",_=>b"Off"}
}
fn sleep_setting_label(p:u8)->&'static [u8]{
    match p{1=>b"Sleep Screen (Server)",2=>b"Sleep Screen (Device)",3=>b"Sleep Screen (Book)",
        4=>b"Sleep Screen (Week)",5=>b"Sleep Screen (Calendar)",_=>b"Sleep Screen"}
}
impl Ui {
    #[inline(never)]
    fn sleep_picker(&self,selected:u8,paired:bool) {
        dsn_clear();self.header(7);
        let y=13+self.ls+self.lt;let w=self.w-2*G;
        self.panel(G,y,w,self.content_bottom()-y-13);
        text(G+8,y+8,w-16,self.lt,b"Choose Sleep Screen",TITLE,CENTER,BOLD);
        for i in 0..6u8{
            if i==1 && !paired{continue;}
            let (x,y,w,h)=self.sleep_option(i);
            dsn_stroke(x,y,w,h,BLACK,if selected==i{2}else{1},7,15);
            text(x+4,y+(h-self.ls)/2,w-8,self.ls,sleep_label(i),SMALL,CENTER,BOLD);
        }
    }
    fn sleep_option(&self,i:u8)->(i32,i32,i32,i32){
        let y=25+self.ls+2*self.lt;
        let h=((self.content_bottom()-y-18)/6).min(self.lb+22);
        (G+12,y+i as i32*h,self.w-2*G-24,h-3)
    }
}