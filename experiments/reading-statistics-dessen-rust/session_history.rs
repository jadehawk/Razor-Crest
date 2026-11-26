// CrossPoint session log provider. Read-only, bounded streaming; no native
// binary records are combined with these sessions.
const SESSION_LOG: &[u8] = b"/.crosspoint/reading_sessions.jsonl";
#[derive(Clone, Copy)]
struct LogBook { id: [u8; 32], title: [u8; 80], len: u8, progress: i32 }
const EMPTY_LOG_BOOK: LogBook = LogBook { id: [0;32], title: [0;80], len: 0, progress: -1 };


// Read complete lines only; skip corrupt, oversized and truncated records.
#[inline(never)]
fn session_lines(chunk: &mut [u8], visit: &mut dyn FnMut(&[u8])) {
    // Larger reads amortize the native open/seek/close syscall. Only records
    // crossing a chunk boundary need copying into the carry buffer.
    let mut line = [0u8; 512];
    let (mut offset, mut len, mut overflow) = (0u32, 0usize, false);
    loop {
        let n = dsn_fs_read_at(SESSION_LOG, offset, chunk);
        if n <= 0 || n as usize > chunk.len() { break; }
        let bytes = &chunk[..n as usize];
        let mut begin = 0;
        for (i, &b) in bytes.iter().enumerate() {
            if b != b'\n' { continue; }
            let part = &bytes[begin..i];
            if !overflow && len + part.len() <= line.len() {
                if len == 0 {
                    if !part.is_empty() { visit(part); }
                } else {
                    line[len..len + part.len()].copy_from_slice(part);
                    visit(&line[..len + part.len()]);
                }
            }
            begin = i + 1;
            len = 0; overflow = false;
        }
        let tail = &bytes[begin..];
        if !overflow && len + tail.len() <= line.len() {
            line[len..len + tail.len()].copy_from_slice(tail);
            len += tail.len();
        } else { overflow = true; }
        let Some(next) = offset.checked_add(n as u32) else { break; };
        offset = next;
    }
}

// Field lookup skips quoted strings (including escaped quotes), so a title
// containing a field name cannot masquerade as a numeric field.
#[inline(never)]
fn session_field<'a>(line: &'a [u8], key: &[u8]) -> Option<&'a [u8]> {
    let mut i = 0;
    while i < line.len() {
        if line[i] != b'"' { i += 1; continue; }
        let start = i + 1; i = start;
        while i < line.len() && line[i] != b'"' {
            i += if line[i] == b'\\' { 2 } else { 1 };
        }
        if i >= line.len() { return None; }
        let end = i; i += 1;
        if &line[start..end] != key { continue; }
        while i < line.len() && line[i].is_ascii_whitespace() { i += 1; }
        if line.get(i) != Some(&b':') { continue; }
        i += 1;
        while i < line.len() && line[i].is_ascii_whitespace() { i += 1; }
        return Some(&line[i..]);
    }
    None
}
#[inline(never)]
fn session_num(line: &[u8], key: &[u8]) -> Option<i64> {
    let raw = session_field(line, key)?;
    let neg = raw.first() == Some(&b'-');
    let mut n = 0i64; let mut i = neg as usize;
    while i < raw.len() && raw[i].is_ascii_digit() {
        n = n.checked_mul(10)?.checked_add((raw[i]-b'0') as i64)?; i += 1;
    }
    if i == neg as usize || !matches!(raw.get(i), Some(b',' | b'}' | b' ' | b'\r' | b'\t' | b'\n')) { return None; }
    Some(if neg { -n } else { n })
}
#[inline(never)]
fn session_string<'a>(line: &'a [u8], key: &[u8]) -> Option<&'a [u8]> {
    let raw = session_field(line, key)?;
    if raw.first() != Some(&b'"') { return None; }
    let mut i = 1;
    while i < raw.len() {
        if raw[i] == b'"' { return Some(&raw[1..i]); }
        i += if raw[i] == b'\\' { 2 } else { 1 };
    }
    None
}
#[inline(never)]
fn session_meta(line: &[u8]) -> Option<(&[u8], u32, i64, i64)> {
    if line.first() != Some(&b'{') || !line.ends_with(b"}") { return None; }
    if let Some(kind)=session_string(line,b"type") { if kind!=b"session" { return None; } }
    let id = session_string(line, b"doc")?;
    if id.len()!=32 || !id.iter().all(|b| b.is_ascii_hexdigit()) { return None; }
    let secs = session_num(line,b"secs")?;
    if !(10..=604800).contains(&secs) { return None; }
    let start = session_num(line,b"start").unwrap_or(0);
    let tz = session_num(line,b"tz").filter(|v| (-720..=840).contains(v));
    Some((id, secs as u32, if tz.is_some() {start} else {0}, tz.unwrap_or(0)))
}
fn valid_epoch(t: i64) -> bool { (946684800..=4102444799).contains(&t) }
// Count exact document IDs using bounded sorted batches. Most libraries
// finish in the first scan; very large libraries retain exact counts.
const ID_BATCH: usize = 32;
struct SessionIds {
    ids: [[u8; 32]; ID_BATCH],
    len: usize,
    overflow: bool,
}
impl SessionIds {
    fn new() -> Self { Self { ids: [[0;32];ID_BATCH], len: 0, overflow: false } }
    fn insert(&mut self, id: &[u8], after: Option<&[u8;32]>) {
        if after.map_or(false, |a| id <= a.as_slice()) { return; }
        let pos = self.ids[..self.len].iter().position(|a| a.as_slice() >= id).unwrap_or(self.len);
        if pos < self.len && self.ids[pos].as_slice() == id { return; }
        if self.len == ID_BATCH { self.overflow = true; }
        if pos == ID_BATCH { return; }
        if self.len < ID_BATCH { self.len += 1; }
        self.ids.copy_within(pos..self.len-1, pos+1);
        self.ids[pos].copy_from_slice(id);
    }
}
#[inline(never)]
fn session_inventory(path: &[u8], chunk: &mut [u8]) -> ((i64,i64), [u8;32], u32, bool) {
    let mut latest = (0,0);
    let mut current_id = [0u8;32];
    let mut batch = SessionIds::new();
    let mut has_baseline=false;
    session_lines(chunk, &mut |line| {
        if session_string(line,b"type").map_or(false, |v| v.starts_with(b"rc-baseline-")) { has_baseline=true; }
        if let Some((id,_,start,tz))=session_meta(line) {
            if valid_epoch(start) && start > latest.0 { latest=(start,tz); }
            if path.first()==Some(&b'/') && session_string(line,b"path")==Some(path) {
                current_id.copy_from_slice(id);
            }
            batch.insert(id,None);
        }
    });
    if has_baseline { return ((0,0),[0;32],0,true); }
    let mut total = batch.len as u32;
    while batch.overflow {
        let after = batch.ids[batch.len-1];
        batch.len=0;
        batch.overflow=false;
        session_lines(chunk, &mut |line| {
            if let Some((id,_,_,_))=session_meta(line) {
                batch.insert(id,Some(&after));
            }
        });
        total += batch.len as u32;
    }
    (latest,current_id,total,false)
}
fn civil_from_days(day: u32) -> (u32,u32,u32) {
    let z=day+719468; let era=z/146097; let doe=z%146097;
    let yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
    let y=yoe+era*400; let doy=doe-(365*yoe+yoe/4-yoe/100);
    let mp=(5*doy+2)/153; let d=doy-(153*mp+2)/5+1;
    let m=if mp<10 {mp+3} else {mp-9};
    (y+u32::from(m<=2),m,d)
}
fn log_period(local: i64) -> usize {
    match (local%86400)/3600 {6..=11=>0,12..=16=>1,17..=20=>2,_=>3}
}
 // Count distinct documents in bounded sorted batches. Each pass retains the
 // smallest five IDs after the previous boundary, with latest recorded progress.
 #[inline(never)]
 fn log_book_batch(after: &[u8;32], out: &mut [LogBook;5]) -> usize {
     let mut count=0usize;
     let mut chunk=[0u8;1024];
     session_lines(&mut chunk, &mut |line| {
         let Some((id,_,_,_))=session_meta(line) else {return;};
         if id <= after.as_slice() {return;}
         let pos=out[..count].iter().position(|b| b.id.as_slice()>=id).unwrap_or(count);
         if pos<count && out[pos].id.as_slice()==id {

             if let Some(p)=session_num(line,b"to").filter(|v| (0..=10000).contains(v)) {
                 out[pos].progress=((p+50)/100) as i32;
             }
             return;
         }
         if pos>=out.len() {return;}
         if count<out.len() {count+=1;}
         for j in (pos+1..count).rev() {out[j]=out[j-1];}
         let book=&mut out[pos];
         *book=EMPTY_LOG_BOOK;
         book.id.copy_from_slice(id);

         if let Some(p)=session_num(line,b"to").filter(|v| (0..=10000).contains(v)) {
             book.progress=((p+50)/100) as i32;
         }
         if let Some(title)=session_string(line,b"title") {
             let mut n=title.len().min(80);
             while n>0 && core::str::from_utf8(&title[..n]).is_err() {n-=1;}
             book.title[..n].copy_from_slice(&title[..n]); book.len=n as u8;
         }
     });
     count
 }
 #[inline(never)]
 fn log_books_page(offset: u32, out: &mut [LogBook;5]) -> usize {
     let mut after=[0u8;32];
     let mut skipped=0;
     loop {
         let n=log_book_batch(&after,out);
         if n==0 {return 0;}
         if skipped+n as u32>offset {
             let first=(offset-skipped) as usize;
             out.copy_within(first..n,0);
             return n-first;
         }
         after=out[n-1].id;
         skipped+=n as u32;
     }
 }
#[inline(never)]
fn load_sessions(st: &mut Stats, path: &[u8], current_id: &[u8;32], chunk: &mut [u8]) -> Current {
    let mut current=Current {found:false,secs:0,sessions:0,turns:0,active:0,periods:[0;4]};
    let mut current_days=[false; DAYS];

    session_lines(chunk, &mut |line| {
        let Some((id,secs,start,tz))=session_meta(line) else {return;};
        let tracked=if session_num(line,b"v")==Some(2) {
            match session_num(line,b"tracked_secs") {Some(v) if (secs as i64..=604800).contains(&v)=>v as u32,_=>return}
        } else {secs};
        st.total=st.total.saturating_add(tracked);
        st.sessions=st.sessions.saturating_add(1);
        let is_current=path.first()==Some(&b'/') && (id==current_id || session_string(line,b"path")==Some(path));
        if is_current {current.found=true;current.secs=current.secs.saturating_add(tracked);current.sessions=current.sessions.saturating_add(1);}
        if !valid_epoch(start) {return;}
        let local=start+tz*60;
        let day=(local/86400) as u32; let period=log_period(local);
        st.hour[period]=st.hour[period].saturating_add(secs);
        if is_current {current.periods[period]=current.periods[period].saturating_add(secs);}
        if day<=st.end && st.end-day<DAYS as u32 {
            let pos=DAYS-1-(st.end-day) as usize;
            st.day[pos]=st.day[pos].saturating_add(secs);
            if st.end-day<7 {st.week_hour[period]=st.week_hour[period].saturating_add(secs);}
            if is_current {current_days[pos]=true;}
        }
    });
    current.active=current_days.iter().filter(|&&v|v).count() as u32;
    current
}