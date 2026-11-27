// Preferences only: do not edit the source book or reading history.
// 24-byte record: document ID, latest log offset, live progress, mode, reserved.
const FILTER_FILE:&[u8]=b"/book-list.bin";
struct BookFilter {excluded:u32,hidden:bool,ready:bool}
impl BookFilter {
    fn new()->Self{Self{excluded:0,hidden:false,ready:false}}
    fn count(&self,total:u32)->u32{if self.hidden{self.excluded}else{total.saturating_sub(self.excluded)}}
    #[inline(never)]
    fn index(&self,mut n:u32,total:u32,bits:&[u8])->u32{
        if self.excluded==0{return if self.hidden{total}else{n};}
        for i in 0..total.min(1024){
            if ((bits[i as usize/8]>>(i%8))&1!=0)==self.hidden{
                if n==0{return i;}n-=1;
            }
        }
        total
    }
    #[inline(never)]
    fn load(&mut self,total:u32,scratch:&mut [u8],bits:&mut [u8])->bool{
        bits.fill(0);self.excluded=0;self.ready=true;
        let mut path=[0u8;192];
        let n=StatsSettings::path(&mut path,FILTER_FILE);
        if n==0{return false;}
        let size=dsn_fs_read(&path[..n],scratch);
        if size==-2{return true;}
        if size<0 || size as usize>scratch.len() || size%24!=0{return false;}
        let size=size as usize;
        let mut kept=0;
        let mut id=[0u8;20];
        // Native document IDs are sorted; search only excluded books.
        for pos in (0..size).step_by(24){
            let r=&scratch[pos..pos+24];
            if !matches!(r[22],1|2) || r[23]!=0{return false;}
            let mut lo=0;let mut hi=total.min(1024);
            while lo<hi{
                let mid=(lo+hi)/2;
                if dsn_reading_stats_read(8,mid,&mut id)!=20{return false;}
                if id[..16]<r[..16]{lo=mid+1;}else{hi=mid;}
            }
            if lo<total.min(1024) && dsn_reading_stats_read(8,lo,&mut id)==20 && id[..16]==r[..16]{
                if r[22]==1 && (id[16..20]!=r[16..20] || book_progress_changed(lo,i16::from_le_bytes([r[20],r[21]]) as i32)){
                    continue;
                }
                let bit=1<<(lo%8);
                if bits[lo as usize/8]&bit==0{self.excluded+=1;bits[lo as usize/8]|=bit;}
            }
            scratch.copy_within(pos..pos+24,kept);kept+=24;
        }
        if kept!=size && dsn_fs_write(&path[..n],&scratch[..kept])!=0{return false;}
        true
    }
    #[inline(never)]
    fn change(&mut self,index:u32,mode:u8,scratch:&mut [u8])->bool{
        let mut record=[0u8;24];
        if dsn_reading_stats_read(8,index,&mut record[..20])!=20{return false;}
        record[20..22].copy_from_slice(&(book_row_progress(scratch) as i16).to_le_bytes());
        record[22]=mode;
        let mut path=[0u8;192];
        let n=StatsSettings::path(&mut path,FILTER_FILE);
        if n==0{return false;}
        let size=dsn_fs_read(&path[..n],scratch);
        let mut size=if size==-2{0}else if size>=0 && size%24==0 && size as usize<=scratch.len(){size as usize}else{return false;};
        let mut pos=0;
        while pos<size && scratch[pos..pos+16]!=record[..16]{pos+=24;}
        if pos<size{
            scratch.copy_within(pos+24..size,pos);size-=24;
        }
        if mode!=0{
            if size+24>scratch.len(){return false;}
            scratch[size..size+24].copy_from_slice(&record);size+=24;
        }
        if dsn_fs_write(&path[..n],&scratch[..size])!=0{return false;}
        self.ready=false;
        true
    }
}
fn book_row_path(r:&[u8])->&[u8]{
    let p=&r[92..268];&p[..p.iter().position(|&v|v==0).unwrap_or(p.len())]
}
fn book_row_progress(r:&[u8])->i32{
    let path=book_row_path(r);
    if path.is_empty(){-1}else{dsn_book_progress(path)}
}
#[inline(never)]
fn book_progress_changed(index:u32,previous:i32)->bool{
    let mut row=[0u8;364];
    if previous<0 || dsn_reading_stats_read(9,index,&mut row)!=364{return false;}
    let p=book_row_progress(&row);p>=0 && p!=previous
}
impl Ui{
    fn book_option(&self,i:u8)->(i32,i32,i32,i32){
        let (x,y,w,h)=self.sleep_option(i);
        (x,y+2*self.ls,w,h)
    }
    #[inline(never)]
    fn book_actions(&self,row:&[u8],focus:u8,hidden:bool){
        dsn_clear();self.header(if hidden{8}else{5});
        let y=13+self.ls+self.lt;let w=self.w-2*G;
        self.panel(G,y,w,self.content_bottom()-y-13);
        let title=&row[12..12+row[10].min(80) as usize];
        dsn_text_box(G+8,y+5,w-16,2*self.lt,title,TITLE,CENTER,2,BOLD);
        let choices:[&[u8];4]=if hidden{[b"Restore Book",b"Cancel",b"",b""]}else{[b"Open Book",b"Delete from List",b"Hide Book",b"Cancel"]};
        for i in 0..if hidden{2}else{4}{
            let (x,y,w,h)=self.book_option(i);
            dsn_stroke(x,y,w,h,BLACK,if focus==i{2}else{1},7,15);
            text(x+4,y+(h-self.ls)/2,w-8,self.ls,choices[i as usize],SMALL,CENTER,BOLD);
        }
    }
}