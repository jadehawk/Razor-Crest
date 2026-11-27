// Server protocol belongs to this app, not the native statistics provider.
// All bodies and responses use the existing Home scratch buffer.
use core::convert::TryInto;
const HISTORY_PUT: &[u8] = b"PUT https://sync.techy-notes.com/api/device/reader-history\nContent-Type: application/json\nAuthorization: Bearer ";
const HISTORY_STATE: &[u8] = b"/history-upload-v1";
const HISTORY_OK: &[u8] = b"Reading history uploaded";
const HISTORY_ALREADY_UPLOADED: &[u8] = b"Reading history already uploaded";
const HISTORY_ERROR: &[u8] = b"History upload failed. Retry safely.";
const HISTORY_FOREIGN: &[u8] = b"History belongs to another reader.";
const HISTORY_INVALID: &[u8] = b"Invalid or changed reading history.";
const HISTORY_MEMORY: &[u8] = b"Not enough memory to prepare history. Retry.";
const HISTORY_IO: &[u8] = b"Cannot read or save history. Check SD card.";
type HistoryResult<T> = Result<T, i32>;
#[inline(never)]
fn history_native_error(code:i32)->&'static [u8] {
    match code {
        -6 => HISTORY_MEMORY,
        -5 => HISTORY_FOREIGN,
        -2 => HISTORY_IO,
        -4 => b"History service unavailable.",
        1 => b"No reading history.",
        -7 => b"History network failed. Retry.",
        401 => b"Pairing revoked or expired. Pair again.",
        403 => b"Server denied reader history access.",
        400 => b"Server rejected history payload.",
        409 => b"Server history checkpoint conflict.",
        429 => b"Server busy. Retry history upload.",
        500..=599 => HISTORY_ERROR,
        _ => HISTORY_INVALID,
    }
}
const HASH_INIT: u64 = 14695981039346656037;

fn history_supported() -> bool {
    dsn_reading_stats_read(7,2,&mut []) == 1
}
#[inline(never)]
fn hist_hash(bytes: &[u8]) -> u64 {
    bytes.iter().fold(HASH_INIT, |h,b| (h ^ *b as u64).wrapping_mul(1099511628211))
}
#[inline(never)]
fn hist_u64(b: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(b[at..at+8].try_into().unwrap())
}
#[inline(never)]
fn hist_hex(src: &[u8], out: &mut [u8]) {
    const HEX: &[u8] = b"0123456789abcdef";
    for (i,b) in src.iter().enumerate() {
        out[2*i] = HEX[(b>>4) as usize];
        out[2*i+1] = HEX[(b&15) as usize];
    }
}

#[derive(Clone, Copy)]
struct HistoryCursor {
    account: u64,
    id: [u8;16],
    completed: u64,
    completed_hash: u64,
    target: u64,
    target_hash: u64,
    scope: u32,
    day: u32,
}
impl HistoryCursor {
    fn new(account: u64, id: [u8;16]) -> Self {
        Self { account,id,completed:0,completed_hash:HASH_INIT,target:0,target_hash:0,scope:0,day:0 }
    }
    fn load(account: u64, id: [u8;16]) -> Option<Self> {
        let mut path=[0u8;192];
        let n=StatsSettings::path(&mut path,HISTORY_STATE);
        if n==0 { return None; }
        let mut b=[0u8;76];
        let size=dsn_fs_read(&path[..n],&mut b);
        // Missing cursor means a safe replay of cumulative checkpoints.
        if size<0 { return Some(Self::new(account,id)); }
        if size!=76 || &b[..4]!=b"RHC1" || hist_hash(&b[..68])!=hist_u64(&b,68) { return None; }
        if hist_u64(&b,4)!=account || b[12..28]!=id { return Some(Self::new(account,id)); }
        let c=Self {account,id,completed:hist_u64(&b,28),completed_hash:hist_u64(&b,36),
            target:hist_u64(&b,44),target_hash:hist_u64(&b,52),scope:le32(&b,60),day:le32(&b,64)};
        if c.completed>u32::MAX as u64 || c.target>u32::MAX as u64 || c.scope>1025 ||
            c.day>DAYS as u32 || (c.target!=0 && c.target<=c.completed) ||
            (c.target==0 && (c.scope!=0 || c.day!=0)) { return None; }
        Some(c)
    }
    fn save(&self) -> bool {
        let mut path=[0u8;192];
        let n=StatsSettings::path(&mut path,HISTORY_STATE);
        if n==0 { return false; }
        let mut b=[0u8;76];
        b[..4].copy_from_slice(b"RHC1");
        b[4..12].copy_from_slice(&self.account.to_le_bytes());
        b[12..28].copy_from_slice(&self.id);
        b[28..36].copy_from_slice(&self.completed.to_le_bytes());
        b[36..44].copy_from_slice(&self.completed_hash.to_le_bytes());
        b[44..52].copy_from_slice(&self.target.to_le_bytes());
        b[52..60].copy_from_slice(&self.target_hash.to_le_bytes());
        b[60..64].copy_from_slice(&self.scope.to_le_bytes());
        b[64..68].copy_from_slice(&self.day.to_le_bytes());
        let hash=hist_hash(&b[..68]);
        b[68..].copy_from_slice(&hash.to_le_bytes());
        dsn_fs_write(&path[..n],&b)==0
    }
}

#[derive(Clone, Copy)]
struct HistorySource { id:[u8;16], bytes:u64, hash:u64, owned:bool }
fn history_source() -> HistoryResult<HistorySource> {
    let mut b=[0u8;40];
    let result=dsn_reading_stats_read(7,0,&mut b);
    if result!=40 {return Err(result);}
    if le32(&b,0)!=1 || le32(&b,4)>1 {return Err(-1);}
    let id=b[8..24].try_into().map_err(|_|-1)?;
    let owned=le32(&b,4)==1;
    if owned==(id==[0;16]) {return Err(-1);}
    let bytes=hist_u64(&b,24);
    if bytes>u32::MAX as u64 {return Err(-1);}
    Ok(HistorySource{id,bytes,hash:hist_u64(&b,32),owned})
}
fn history_query(prefix: u64, doc: &[u8]) -> HistoryResult<HistorySource> {
    let mut key=[0u8;50];
    let result=if prefix==u64::MAX { dsn_reading_stats_start(doc,0) }
    else {
        key[0]=b'@';
        hist_hex(&prefix.to_be_bytes(),&mut key[1..17]);
        key[17]=b':';
        key[18..18+doc.len()].copy_from_slice(doc);
        dsn_reading_stats_start(&key[..18+doc.len()],0)
    };
    if result!=0 {return Err(result);}
    history_source()
}
#[inline(never)]
fn history_matches(s: HistorySource,c:&HistoryCursor) -> bool {
    s.owned && s.id==c.id && s.bytes==c.target && s.hash==c.target_hash
}

// Bounded JSON writer. Never send a truncated object.
struct HistoryJson<'a> { data:&'a mut [u8], n:usize, ok:bool }
impl<'a> HistoryJson<'a> {
    fn new(data:&'a mut [u8])->Self {Self{data,n:0,ok:true}}
    #[inline(never)]
    fn raw(&mut self,b:&[u8]) {
        if !self.ok || b.len()>self.data.len()-self.n {self.ok=false;return;}
        self.data[self.n..self.n+b.len()].copy_from_slice(b);self.n+=b.len();
    }
    #[inline(never)]
    fn number(&mut self,mut v:u64) {
        if v>9_007_199_254_740_991 {self.ok=false;return;}
        let mut b=[0u8;20];let mut i=b.len();
        loop {i-=1;b[i]=b'0'+(v%10) as u8;v/=10;if v==0{break;}}
        self.raw(&b[i..]);
    }
    #[inline(never)]
    fn field(&mut self,name:&[u8],value:u64) {self.raw(b",\"");self.raw(name);self.raw(b"\":");self.number(value);}
    #[inline(never)]
    fn quoted(&mut self,name:&[u8],value:&[u8]) {self.raw(b",\"");self.raw(name);self.raw(b"\":\"");self.raw(value);self.raw(b"\"");}
    fn finish(self)->Option<usize>{self.ok.then_some(self.n)}
}
fn history_date(day:u32)->[u8;10] {
    let (y,m,d)=civil_from_days(day);
    [b'0'+(y/1000%10) as u8,b'0'+(y/100%10) as u8,b'0'+(y/10%10) as u8,b'0'+(y%10) as u8,b'-',
     b'0'+(m/10) as u8,b'0'+(m%10) as u8,b'-',b'0'+(d/10) as u8,b'0'+(d%10) as u8]
}

// A validated JSONL record contains escaped string values. Preserve their
// JSON escaping, omit control escapes and strings exceeding the byte budget.
fn history_metadata(w:&mut HistoryJson, record:&[u8], doc:&[u8]) {
    if session_string(record,b"doc").map_or(true,|v| !v.eq_ignore_ascii_case(doc)) {return;}
    let mut have_series=false;
    for key in [b"title".as_slice(),b"author",b"series",b"series_index"] {
        let Some(v)=session_string(record,key) else {continue;};
        if v.is_empty() || v.len()>80 || v.iter().all(|b| *b==b' ') || core::str::from_utf8(v).is_err(){continue;}
        let mut i=0;
        let mut safe=true;
        while i<v.len() {
            if v[i]<32 || v[i]==127 {safe=false;break;}
            if v[i]==b'\\' {
                i+=1;
                if !matches!(v.get(i),Some(b'"'|b'\\'|b'/')) {safe=false;break;}
            }
            i+=1;
        }
        if !safe {continue;}
        if key==b"series_index" {
            let mut parts=v.split(|b| *b==b'.');
            let whole=parts.next().unwrap_or(b"");
            let fraction=parts.next();
            if !have_series || whole.is_empty() || whole.len()>5 || !whole.iter().all(u8::is_ascii_digit) ||
                fraction.map_or(false,|f| f.is_empty()||f.len()>6||!f.iter().all(u8::is_ascii_digit)) || parts.next().is_some(){continue;}
            let number=whole.iter().fold(0u32,|n,b|n*10+(*b-b'0') as u32);
            if number>10000 || number==10000 && fraction.map_or(false,|f|f.iter().any(|b|*b!=b'0')){continue;}
        }
        w.quoted(key,v);
        if key==b"series" {have_series=true;}
    }
}
fn history_ack(payload:&[u8], id:&[u8], work:&[(u32,[u8;32],u32)], target:u64)->bool {
    if session_num(payload,b"v")!=Some(2) || session_string(payload,b"history_id")!=Some(id){return false;}
    let Some(device)=session_string(payload,b"device_id") else{return false;};
    if device.len()!=23 || !device.starts_with(b"crosspoint-") ||
        !device[11..].iter().all(|b|b.is_ascii_digit()||matches!(b,b'a'..=b'f')){return false;}
    let Some(accepted)=session_field(payload,b"accepted") else{return false;};
    let mut row=accepted.trim_ascii_start();
    if row.first()!=Some(&b'['){return false;}
    row=&row[1..];
    for (i,(scope,doc,_)) in work.iter().enumerate(){
        row=row.trim_ascii_start();
        if row.first()!=Some(&b'{'){return false;}
        let Some(end)=row.iter().position(|b|*b==b'}') else{return false;};
        let item=&row[..=end];
        let document_ok=if *scope==0{session_field(item,b"document").map_or(false,|v|v.starts_with(b"null") &&
            matches!(v.get(4),Some(b','|b'}'|b' '|b'\r'|b'\n'|b'\t')))}
            else{session_string(item,b"document")==Some(doc)};
        if !document_ok || session_num(item,b"checkpoint")!=Some(target as i64){return false;}
        row=row[end+1..].trim_ascii_start();
        if i+1<work.len(){if row.first()!=Some(&b','){return false;}row=&row[1..];}
    }
    row.first()==Some(&b']')
}
#[inline(never)]
fn history_put(pairing:&Pairing, buffer:&mut [u8], used:usize)->HistoryResult<usize>{
    let mut head=[0u8;224];
    let n=HISTORY_PUT.len();
    head[..n].copy_from_slice(HISTORY_PUT);
    head[n..n+72].copy_from_slice(&pairing.access_token);
    head[n+72]=b'\n';
    if used>buffer.len() || buffer.len()<4{return Err(-1);}
    if dsn_wifi_connect()<0{return Err(-7);}
    // Current history-identity-capable host stages the full request before
    // publishing the response, including when it detaches the guest for a
    // network reboot. Use one mutable slice, not aliased Rust references.
    let ptr=buffer.as_mut_ptr() as usize;
    let received=unsafe{dsn_call6(DSN_SYS_NET_REQUEST,head.as_ptr() as usize,n+73,
                                  ptr,used,ptr,buffer.len())};
    if received<0{return Err(-7);}
    let size=received as usize;
    let status=u16::from_be_bytes([buffer[0],buffer[1]]) as i32;
    let headers=u16::from_be_bytes([buffer[2],buffer[3]]) as usize;
    if status!=200{return Err(status);}
    if size>buffer.len()-4 || headers>buffer.len()-4-size{return Err(-1);}
    move_response(buffer,4+headers,size);
    Ok(size)
}

// Build a bounded item from a fixed native history prefix without networking.
// Only the in-memory candidate cursor advances until the batch is acknowledged.
#[inline(never)]
fn history_build_item(c:&mut HistoryCursor,doc:&[u8],offset:u32,
                     body:&mut [u8],temp:&mut [u8])->HistoryResult<usize> {
    let source=history_query(c.target,doc)?;
    if !history_matches(source,c){return Err(-1);}
    let mut summary=[0u8;192];let mut turns=[0u8;24];
    if dsn_reading_stats_read(1,0,&mut summary)!=192 || le32(&summary,0)!=1 || le32(&summary,4)!=0 ||
        hist_u64(&summary,32)!=c.target || hist_u64(&summary,40)!=0 ||
        dsn_reading_stats_read(6,0,&mut turns)!=24 || le32(&turns,0)!=1{return Err(-1);}
    let book=!doc.is_empty();
    let base=if book{104}else{48};
    let mut w=HistoryJson::new(body);
    w.raw(b"{\"checkpoint\":");w.number(c.target);
    if book{w.quoted(b"document",doc);}
    w.field(b"seconds",hist_u64(&summary,base));w.field(b"sessions",hist_u64(&summary,base+8));
    w.field(b"turns",hist_u64(&turns,if book{16}else{8}));
    w.raw(b",\"turns_complete\":");w.raw(if le32(&turns,4)&(if book{2}else{1})!=0{b"true"}else{b"false"});
    w.raw(b",\"periods\":[");
    for i in 0..4 {if i!=0{w.raw(b",");}w.number(hist_u64(&summary,base+24+i*8));}
    w.raw(b"],\"daily\":[");
    let anchor=le32(&summary,8);
    if anchor!=0 && !(10957..=47482).contains(&anchor){return Err(-1);}
    let start=anchor.saturating_sub(DAYS as u32-1).max(10957);
    let days=if anchor>=start{anchor-start+1}else{0};
    let mut next=c.day;let mut count=0;
    if next>days{return Err(-1);}
    // Read small chunks; if a page fills mid-chunk the next page resumes
    // at the first unconsumed day.
    while next<days && count<20 {
        let n=(days-next).min(32) as usize;
        if dsn_reading_stats_read(2,start+next,&mut temp[..n*16])!=(n*16) as i32{return Err(-1);}
        for i in 0..n {
            let seconds=hist_u64(temp,i*16+if book{8}else{0});
            let date=start+next;next+=1;
            if seconds==0{continue;}
            if seconds>u32::MAX as u64{return Err(-1);}
            if count!=0{w.raw(b",");}count+=1;
            w.raw(b"{\"date\":\"");w.raw(&history_date(date));w.raw(b"\",\"seconds\":");w.number(seconds);w.raw(b"}");
            if count==20{break;}
        }
    }
    w.raw(b"]");
    if book {
        if offset==u32::MAX || offset as u64>=c.target{return Err(-1);}
        let want=((c.target-offset as u64) as usize).min(513);
        let n=dsn_fs_read_at(SESSION_LOG,offset,&mut temp[..want]);
        if n<=0 || n as usize>want{return Err(-1);}
        let Some(end)=temp[..n as usize].iter().position(|b|*b==b'\n') else{return Err(-1);};
        history_metadata(&mut w,&temp[..end],doc);
    }
    w.raw(b"}");
    let Some(n)=w.finish() else{return Ok(0);};
    c.day=next;
    if next==days{c.scope+=1;c.day=0;}
    Ok(n)
}

#[inline(never)]
fn upload_history(pairing:&Pairing,server:&mut ServerOverall,body:&mut [u8],scratch:&mut [u8],
                  refreshed:&mut Option<bool>)->&'static [u8] {
    *refreshed=None;
    if !history_supported() || pairing.phase!=PairPhase::Paired || scratch.len()<513{return HISTORY_ERROR;}
    // Token rotation starts a safe cumulative replay. The server derives the
    // physical ID from the authenticated hardware header, without a probe.
    let account=hist_hash(&pairing.access_token);
    let mut source=match history_query(u64::MAX,b""){Ok(s)=>s,Err(e)=>return history_native_error(e)};
    if !source.owned {
        let rc=dsn_reading_stats_read(7,1,&mut []);
        if rc!=0{return history_native_error(rc);}
        source=match history_query(u64::MAX,b""){Ok(s)=>s,Err(e)=>return history_native_error(e)};
    }
    if !source.owned{return HISTORY_INVALID;}
    let Some(mut cursor)=HistoryCursor::load(account,source.id) else{return b"Invalid history upload cursor";};
    if cursor.target==0 {
        if cursor.completed>source.bytes{return HISTORY_INVALID;}
        if cursor.completed!=0 {
            if cursor.completed==source.bytes {
                if cursor.completed_hash!=source.hash{return HISTORY_INVALID;}
                let _=dsn_reading_stats_read(4,0,&mut []);
                *refreshed=Some(server.fetch(pairing,body));
                return HISTORY_ALREADY_UPLOADED;
            }
            let old=match history_query(cursor.completed,b""){Ok(s)=>s,Err(e)=>return history_native_error(e)};
            if !old.owned || old.id!=source.id || old.hash!=cursor.completed_hash || old.bytes!=cursor.completed{return HISTORY_INVALID;}
        }
        cursor.target=source.bytes;cursor.target_hash=source.hash;cursor.scope=0;cursor.day=0;
        if !cursor.save(){return HISTORY_ERROR;}
    }
    let mut id=[0u8;32];hist_hex(&cursor.id,&mut id);
    // Preparation and receipt parsing never overlap in time.
    // Share the same bounded buffer for both.
    loop {
        let pinned=match history_query(cursor.target,b""){Ok(s)=>s,Err(e)=>return history_native_error(e)};
        if !history_matches(pinned,&cursor){return HISTORY_INVALID;}
        let mut summary=[0u8;192];
        if dsn_reading_stats_read(1,0,&mut summary)!=192 || le32(&summary,4)!=0 || hist_u64(&summary,40)!=0{return HISTORY_INVALID;}
        let books=le32(&summary,24);
        if books>1024 || cursor.scope>books+1{return HISTORY_INVALID;}
        // A small work list bounds stack usage; more scopes continue automatically.
        let mut work=[(0u32,[0u8;32],u32::MAX);7];
        let mut next=cursor.scope;let mut count=0;
        while next<=books && count<work.len(){
            let mut doc=[0u8;32];let mut offset=u32::MAX;
            if next!=0{
                let mut record=[0u8;20];
                if dsn_reading_stats_read(8,next-1,&mut record)!=20{return HISTORY_ERROR;}
                offset=le32(&record,16);
                if offset==u32::MAX || offset as u64>=cursor.target{return HISTORY_INVALID;}
                if !(next==cursor.scope && cursor.day!=0) && (offset as u64)<cursor.completed{next+=1;continue;}
                hist_hex(&record[..16],&mut doc);
            }
            work[count]=(next,doc,offset);count+=1;next+=1;
        }
        let mut candidate=cursor;
        let mut w=HistoryJson::new(body);
        w.raw(b"{\"v\":2");w.quoted(b"history_id",&id);w.raw(b",\"items\":[");
        let Some(mut used)=w.finish() else{return HISTORY_ERROR;};
        let mut sent=0;
        for &(scope,doc,offset) in &work[..count]{
            if candidate.scope!=scope{
                if candidate.day!=0 || candidate.scope>scope{return HISTORY_ERROR;}
                candidate.scope=scope;
            }
            let mut item_cursor=candidate;
            let comma=usize::from(sent!=0);
            if used+comma+2>=body.len(){break;}
            let limit=body.len()-2;
            let key=if scope==0{b"".as_slice()}else{&doc[..]};
            let n=match history_build_item(&mut item_cursor,key,offset,&mut body[used+comma..limit],scratch){
                Ok(n)=>n,Err(e)=>return history_native_error(e)
            };
            if n==0{break;}
            if comma!=0{body[used]=b',';}
            used+=comma+n;sent+=1;candidate=item_cursor;
            if candidate.day!=0{break;}
        }
        if sent!=0 {
            body[used..used+2].copy_from_slice(b"]}");
            let _=dsn_reading_stats_read(4,0,&mut []);
            let size=match history_put(pairing,body,used+2){Ok(n)=>n,Err(e)=>return history_native_error(e)};
            if !history_ack(&body[..size],&id,&work[..sent],cursor.target){return HISTORY_ERROR;}
            if !candidate.save(){return HISTORY_ERROR;}
            cursor=candidate;
            *refreshed=Some(server.accept_overview(body,size));
        } else if count!=0 {return HISTORY_ERROR;}
        if next>books && sent==count && (count==0 || cursor.scope>work[count-1].0){
            cursor.completed=cursor.target;cursor.completed_hash=cursor.target_hash;
            cursor.target=0;cursor.target_hash=0;cursor.scope=0;cursor.day=0;
            if !cursor.save(){return HISTORY_ERROR;}
            if refreshed.is_none(){
                let _=dsn_reading_stats_read(4,0,&mut []);
                *refreshed=Some(server.fetch(pairing,body));
            }
            return HISTORY_OK;
        }
    }
}
