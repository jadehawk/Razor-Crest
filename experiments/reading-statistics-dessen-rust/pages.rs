// Additional offline pages matching the original Reading Statistics widget order.
// All rendering is bounded by Dessen's safe-area dimensions.

fn weekly_periods(st: &Stats) -> [u32; 4] {
    if st.log_source { return st.week_hour; }
    let mut out = [0u32; 4];
    let mut bytes = [0u8; 148];
    if !(read_native(PERIODS, &mut bytes, 0x31505452, 1) ||
         read_native(PERIODS_BACKUP, &mut bytes, 0x31505452, 1)) { return out; }
    let latest = le32(&bytes, 12);
    if latest == 0 { return out; }
    for ago in 0..7u32 {
        let ordinal = st.end.saturating_add(1).saturating_sub(ago);
        if ordinal == 0 || ordinal > latest || latest - ordinal >= 7 { continue; }
        let off = 32 + (ordinal % 7) as usize * 16;
        for k in 0..4 { out[k] = out[k].saturating_add(le32(&bytes, off + k * 4)); }
    }
    out
}

fn latest_day_seconds(st: &Stats) -> (u32, u32) {
    for ago in 0..DAYS {
        let secs = st.day[DAYS - 1 - ago];
        if secs > 0 { return (secs, ago as u32); }
    }
    (0, 0)
}

// Calendar arithmetic uses the same local-day ordinal as the native records.
fn days_in_month(year: u32, month: u32) -> u32 {
    let leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    match month {
        2 => if leap { 29 } else { 28 },
        4 | 6 | 9 | 11 => 30,
        _ => 31,
    }
}

fn book_at(idx: u32, record: &mut [u8; 316]) -> bool {
    if idx >= 2048 || dsn_fs_read_at(BOOK_INDEX, idx * 316, record) != 316 { return false; }
    record_valid(record, 0x314b4252, 1) &&
        record[16..192].contains(&0) && record[192..264].contains(&0)
}

#[inline(never)]
fn book_time(record: &[u8; 316]) -> Option<u32> {
    let hash = (le32(record, 12) as u64) << 32 | le32(record, 8) as u64;
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut name = [0u8; 80];
    let prefix = BOOK_PREFIX.len();
    name[..prefix].copy_from_slice(BOOK_PREFIX);
    for j in 0..16 { name[prefix + j] = HEX[((hash >> (60 - j * 4)) & 15) as usize]; }
    let end = prefix + 16;
    name[end..end + BOOK_SUFFIX.len()].copy_from_slice(BOOK_SUFFIX);
    let mut snap = [0u8; 1500];
    let ok = read_native(&name[..end + BOOK_SUFFIX.len()], &mut snap, 0x32545352, 2);
    if !ok {
        name[end..end + BOOK_SUFFIX_BACKUP.len()].copy_from_slice(BOOK_SUFFIX_BACKUP);
        if !read_native(&name[..end + BOOK_SUFFIX_BACKUP.len()], &mut snap, 0x32545352, 2) {
            return None;
        }
    }
    Some((((le32(&snap, 20) as u64) << 32) | le32(&snap, 16) as u64)
         .min(u32::MAX as u64) as u32)
}

impl Ui {
    #[inline(never)]
    fn books_capacity(&self) -> u32 {
        let y=13+self.ls+self.lt;
        let available=(self.content_bottom()-y-13).max(1);
        // Round down to a multiple of five to reuse the bounded log batches.
        ((available-self.lb-self.ls-38)/(self.lb+12)).max(1) as u32
    }

    #[inline(never)]
    fn book_row(&self, x:i32,y:i32,w:i32,h:i32,title:&[u8],progress:i32) {
        let pw=((dsn_measure_text(b"100%",SMALL,0)>>16) as i32)+10;
        let p=if (0..=100).contains(&progress) { S::new().n(progress as u32).s(b"%") } else { S::new().s(b"--") };
        text(x+12,y+2,w-pw-24,self.lb,title,BODY,LEFT,BOLD);
        text(x+w-pw-12,y+2,pw,self.ls,p.get(),SMALL,RIGHT,0);
        dsn_line(x+12,y+h-3,x+w-12,y+h-3,1,LIGHT);
    }
    #[inline(never)]
    fn extra_page(&self, st: &Stats, page: i32, year: u32, month: u32, day: u32, book_offset: u32) {
        let x = G;
        let w = self.w - G * 2;
        let y = 13 + self.ls + self.lt;
        let available = (self.content_bottom() - y - 13).max(1);
        match page {
            2 => {
                let gap = 8;
                let chart_h = (available * 47 / 100).max(80);
                let period_h = (available * 32 / 100)
                    .max(self.periods_min_height())
                    .min((available - chart_h - 2 * gap - self.lt - 16).max(1));
                let last_h = (available - chart_h - period_h - 2 * gap).max(self.lt + 16);
                self.week_chart(x, y, w, chart_h, st);
                self.periods(x, y + chart_h + gap, w, period_h, &weekly_periods(st), st.log_source);
                let by = y + chart_h + gap + period_h + gap;
                self.panel(x, by, w, last_h);
                let (seconds, ago) = latest_day_seconds(st);
                text(x + 12, by + 7, w - 24, self.lb, b"Last Read Day", BODY, LEFT, BOLD);
                let value = if seconds > 0 { dur(seconds) } else { S::new().s(b"--") };
                text(x + 12, by + last_h / 2, w / 2, self.lt, value.get(), TITLE, LEFT, BOLD);
                if seconds > 0 {
                    let label = if !st.date_current { let (y,m,d)=civil_from_days(st.end-ago); S::new().n(y).s(b"-").n(m).s(b"-").n(d) } else if ago == 0 { S::new().s(b"Today") } else if ago == 1 {
                        S::new().s(b"Yesterday")
                    } else { S::new().n(ago).s(b" days ago") };
                    text(x + w / 2, by + last_h / 2, w / 2 - 12, self.lb, label.get(), SMALL, RIGHT, 0);
                }
            }
            3 => self.calendar_page(st, year, month, day),
            4 => {
                self.panel(x, y, w, available);
                let title = S::new().s(b"Books tracked: ").n(st.book_count).s(if st.books_limited { b"+" } else { b"" });
                text(x + 12, y + 8, w - 24, self.lb, title.get(), BODY, LEFT, BOLD);
                dsn_line(x + 10, y + self.lb + 14, x + w - 10, y + self.lb + 14, 1, DARK);
                let top = y + self.lb + 20;
                let row_h = self.lb + 12;
                let capacity = self.books_capacity();
                let mut record = [0u8; 316];
                let mut shown = 0u32;
                let mut valid = 0u32;
                if st.log_source {
                    let mut batch=[EMPTY_LOG_BOOK;5];
                    while shown < capacity {
                    let count=log_books_page(book_offset+shown,&mut batch);
                    if count==0 {break;}
                    for book in &batch[..count.min((capacity-shown) as usize)] {
                        let yy=top+shown as i32*row_h;
                        let title=if book.len>0 { &book.title[..book.len as usize] } else { b"Untitled" };
                        self.book_row(x,yy,w,row_h,title,book.progress);
                        shown+=1;
                    }

                    }
                } else { for index in 0..2048 {
                    if !book_at(index, &mut record) { break; }
                    if book_time(&record).is_some() {
                        if valid >= book_offset && shown < capacity {
                            let yy = top + shown as i32 * row_h;
                            let len = record[192..264].iter().position(|&c| c == 0).unwrap_or(72);
                            let title = if len > 0 { &record[192..192 + len] } else { b"Untitled" };
                            let path_len=record[16..192].iter().position(|&c| c==0).unwrap_or(0);
                            let progress=dsn_book_progress(&record[16..16+path_len]);
                            self.book_row(x,yy,w,row_h,title,progress);
                            shown += 1;
                        }
                        valid += 1;
                        if shown >= capacity { break; }
                    }
                }
                }
                if shown == 0 {
                    self.centered(x + 12, top + self.lb, w - 24, self.lb,
                                  b"No more books to display", false);
                }
                // The headline already conveys the count when all books fit.
                if st.book_count > capacity {
                    let first = book_offset + 1;
                    let last = (book_offset + shown).min(st.book_count);
                    let end = S::new().s(b"Showing ").n(first).s(b"-").n(last)
                        .s(b" of ").n(st.book_count).s(b" books");
                    text(x + 12, y + available - self.ls - 5, w - 24, self.ls,
                         end.get(), SMALL, CENTER, 0);
                }
            }
            _ => {}
        }
    }
}
