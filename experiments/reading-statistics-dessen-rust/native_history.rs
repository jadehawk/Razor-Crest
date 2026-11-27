// Optional firmware provider, using the versioned little-endian ABI.
// Older runtimes reject the probe and continue using the JSONL scanner.
fn native_available() -> bool {
    dsn_reading_stats_read(0, 0, &mut []) == 1
}

fn native_u32(data: &[u8], at: usize) -> u32 {
    if le32(data, at + 4) != 0 { u32::MAX } else { le32(data, at) }
}

#[inline(never)]
fn native_load(st: &mut Stats, book: &[u8], anchor: u32, scratch: &mut [u8]) -> Option<Current> {
    let status = dsn_reading_stats_start(book, anchor);
    if status != 0 {
        if status != 1 { st.err=ERR_UNAVAILABLE; }
        return None;
    }
    let mut summary = [0u8; 192];
    if dsn_reading_stats_read(1, 0, &mut summary) != 192 || le32(&summary, 0) != 1 {
        st.err=ERR_UNAVAILABLE;
        let _ = dsn_reading_stats_read(4, 0, &mut []);
        return None;
    }
    let end = le32(&summary, 8);
    if end != 0 && !(10957..=47482).contains(&end) {
        st.err=ERR_UNAVAILABLE;
        let _ = dsn_reading_stats_read(4, 0, &mut []);
        return None;
    }
    if end != 0 {
        let mut i = 0;
        while i < DAYS {
            let n = (DAYS - i).min(64);
            let first = end - (DAYS - 1 - i) as u32;
            if dsn_reading_stats_read(2, first, &mut scratch[..n*16]) != (n*16) as i32 {
                st.err=ERR_UNAVAILABLE;
                st.day.fill(0);
                let _ = dsn_reading_stats_read(4, 0, &mut []);
                return None;
            }
            for j in 0..n { st.day[i+j] = native_u32(scratch, j*16); }
            i += n;
        }
    }
    st.end = end;
    st.book_count = le32(&summary, 24);
    st.books_limited = le32(&summary, 4) & 1 != 0;
    st.total = native_u32(&summary, 48);
    st.sessions = native_u32(&summary, 56);
    for i in 0..4 {
        st.hour[i] = native_u32(&summary, 72+i*8);
        st.week_hour[i] = native_u32(&summary, 160+i*8);
    }
    st.native_stats = true;
    let mut detail = Current { found: !book.is_empty() && native_u32(&summary,112)>0,
        secs: native_u32(&summary,104), sessions: native_u32(&summary,112),
        turns: 0, active: le32(&summary,20), periods: [0;4] };
    for i in 0..4 { detail.periods[i] = native_u32(&summary,128+i*8); }
    let mut turns = [0u8;24];
    if dsn_reading_stats_read(6,0,&mut turns)==24 && le32(&turns,0)==1 {
        let flags=le32(&turns,4);
        if flags&1!=0 { st.progress_bp=native_u32(&turns,8); }
        if flags&2!=0 { detail.turns=native_u32(&turns,16); }
    }
    Some(detail)
}

// Network operations can release the native snapshot to make room for Wi-Fi.
#[inline(never)]
fn native_ensure_books(anchor: u32, scratch: &mut [u8]) -> bool {
    if dsn_reading_stats_read(1,0,&mut scratch[..192]) == 192 { return true; }
    loading_history();
    dsn_reading_stats_start(&[],anchor) == 0
}

#[inline(never)]
fn native_book_batch(first: u32, books: &mut [LogBook;2], scratch: &mut [u8]) -> Option<usize> {
    let n = dsn_reading_stats_read(5,first,&mut scratch[..200]);
    if n < 0 || n > 200 || n % 100 != 0 { return None; }
    for i in 0..(n as usize/100) {
        let r=&scratch[i*100..(i+1)*100];
        let mut b=EMPTY_LOG_BOOK;
        let len=(r[18] as usize).min(80);
        if core::str::from_utf8(&r[20..20+len]).is_ok() {
            b.title[..len].copy_from_slice(&r[20..20+len]);
            b.len=len as u8;
        }
        let progress=u16::from_le_bytes([r[16],r[17]]) as i32;
        if progress<=10000 { b.progress=(progress+50)/100; }
        books[i]=b;
    }
    Some(n as usize/100)
}

fn loading_history() {
    let scr=dsn_screen();
    let (w,h)=((scr>>16) as i32,(scr&0xffff) as i32);
    dsn_clear();
    text(18,h/2-20,w-36,24,b"Loading reading history...",TITLE,CENTER,BOLD);
    text(18,h/2+12,w-36,22,b"Hold Back to cancel",SMALL,CENTER,0);
    dsn_refresh();
}
