// Additional offline pages matching the original Reading Statistics widget order.
// All rendering is bounded by Dessen's safe-area dimensions.

fn weekly_periods(st: &Stats) -> [u32; 4] { st.week_hour }

fn latest_day_seconds(st: &Stats) -> (u32, u32) { (st.latest_secs,st.latest_ago) }

// Calendar arithmetic uses the same local-day ordinal as the native records.
fn days_in_month(year: u32, month: u32) -> u32 {
    let leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    match month {
        2 => if leap { 29 } else { 28 },
        4 | 6 | 9 | 11 => 30,
        _ => 31,
    }
}

impl Ui {
    #[inline(never)]
    fn books_capacity(&self) -> u32 {
        let y=13+self.ls+self.lt;
        let available=(self.content_bottom()-y-13).max(1);
        // Page capacity is independent of the bounded two-book read batches.
        ((available-self.lb-self.ls-31)/self.book_row_height()).max(1) as u32
    }

    #[inline(never)]
    fn book_row_height(&self) -> i32 {
        if self.rich_books { (self.ls * 5 + 6).max(76) } else { self.lb + 12 }
    }

    #[inline(never)]
    fn detailed_book_row(&self, x:i32,y:i32,w:i32,h:i32, first:u32, scratch:&mut [u8]) -> i32 {
        let (r,cover)=scratch.split_at_mut(364);
        let n=dsn_reading_stats_read(9,first,r);
        if n != 364 {return n;}
        let title=&r[12..12+(r[10] as usize).min(80)];
        let path=&r[92..268]; let path=&path[..path.iter().position(|&b| b==0).unwrap_or(path.len())];
        let series=&r[268..364]; let series=&series[..series.iter().position(|&b| b==0).unwrap_or(series.len())];
        let old=u16::from_le_bytes([r[8],r[9]]) as i32;
        let live=if path.is_empty(){-1}else{dsn_book_progress(path)};
        let p=if live>=0 {live} else if old<=10000 {(old+50)/100} else {-1};
        let ch=(h-6).min(144).min(w*3/8); let cw=ch*2/3;
        let bytes=if path.is_empty(){0}else{dsn_book_cover(path,ch,cover)};
        if bytes>4 && bytes as usize<=cover.len() {
            let bw=u16::from_le_bytes([cover[0],cover[1]]) as i32;
            let bh=u16::from_le_bytes([cover[2],cover[3]]) as i32;
            if bw>0 && bh>0 && bw<=cw && bh<=ch && ((bw+7)/8*bh) as usize <= bytes as usize-4 {
                dsn_bitmap(x+8+(cw-bw)/2,y+(h-bh)/2,bw,bh,&cover[4..bytes as usize],bw,bh,1,0,BLACK,0);
            } else {dsn_stroke(x+8,y+(h-ch)/2,cw,ch,DARK,1,0,0);}
        } else {dsn_stroke(x+8,y+(h-ch)/2,cw,ch,DARK,1,0,0);}
        let tx=x+cw+16; let tw=(w-cw-24).max(1);
        let title=if title.is_empty(){b"Untitled".as_slice()}else{title};
        let tl=if (dsn_measure_text(title,SMALL,BOLD)>>16) as i32>tw{2}else{1};
        let sl=if series.is_empty(){0}else if (dsn_measure_text(series,SMALL,0)>>16) as i32>tw{2}else{1};
        let top=y+(h-(tl+sl+1)*self.ls)/2;
        dsn_text_box(tx,top,tw,self.ls*tl,title,SMALL,LEFT,tl,BOLD);
        let sy=top+self.ls*tl;
        if sl>0{dsn_text_box(tx,sy,tw,self.ls*sl,series,SMALL,LEFT,sl,0);}
        let ty=sy+self.ls*sl;
        let minutes=native_u32(r,0)/60;
        let (hours,m)=(minutes/60,minutes%60);
        let time=label!(s(b"Time: "),s(if hours<10{b"0"}else{b""}),n(hours),s(b"h "),s(if m<10{b"0"}else{b""}),n(m),s(b"m"));
        let val=if p>=0{label!(n(p as u32),s(b"%"))}else{label!(s(b"--"))};
        let pw=((dsn_measure_text(b"100%",SMALL,0)>>16) as i32)+8;
        text(tx,ty,tw-pw-4,self.ls,time.get(),SMALL,LEFT,0);
        dsn_line(tx+tw-pw-2,ty,tx+tw-pw-2,ty+self.ls,1,LIGHT);
        text(tx+tw-pw,ty,pw,self.ls,val.get(),SMALL,RIGHT,0);
        dsn_line(x+8,y+h-2,x+w-8,y+h-2,1,LIGHT);
        1
    }

    #[inline(never)]
    fn book_row(&self, x:i32,y:i32,w:i32,h:i32,title:&[u8],progress:i32) {
        let pw=((dsn_measure_text(b"100%",SMALL,0)>>16) as i32)+10;
        let p=if (0..=100).contains(&progress) { label!(n(progress as u32), s(b"%")) } else { label!(s(b"--")) };
        text(x+12,y+2,w-pw-24,self.lb,title,BODY,LEFT,BOLD);
        text(x+w-pw-12,y+2,pw,self.ls,p.get(),SMALL,RIGHT,0);
        dsn_line(x+12,y+h-3,x+w-12,y+h-3,1,LIGHT);
    }
    #[inline(never)]
    fn extra_page(&self, st: &Stats, page: i32, book_offset: u32, selected:u32, books_ready: bool, filter:&BookFilter, bits:&[u8], scratch: &mut [u8]) {
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
                let value = if seconds > 0 { dur(seconds) } else { label!(s(b"--")) };
                text(x + 12, by + last_h / 2, w / 2, self.lt, value.get(), TITLE, LEFT, BOLD);
                if seconds > 0 {
                    let label = if !st.date_current { let (y,m,d)=civil_from_days(st.end-ago); label!(n(y), s(b"-"), n(m), s(b"-"), n(d)) } else if ago == 0 { label!(s(b"Today")) } else if ago == 1 {
                        label!(s(b"Yesterday"))
                    } else { label!(n(ago), s(b" days ago")) };
                    text(x + w / 2, by + last_h / 2, w / 2 - 12, self.lb, label.get(), SMALL, RIGHT, 0);
                }
            }
            3 => {},
            4 => {
                self.panel(x, y, w, available);
                let count=filter.count(st.book_count);
                let title = label!(s(if filter.hidden{b"Hidden: "}else{b"Books tracked: "}), n(count), s(if st.books_limited { b"+" } else { b"" }));
                text(x + 12, y + 8, w - 24, self.lb, title.get(), BODY, LEFT, BOLD);
                dsn_line(x + 10, y + self.lb + 14, x + w - 10, y + self.lb + 14, 1, DARK);
                let top = y + self.lb + 20;
                let row_h = self.book_row_height();
                let capacity = self.books_capacity();
                let mut shown = 0u32;
                let mut failed = !books_ready;
                if st.err != ERR_UNAVAILABLE && self.rich_books && st.native_stats {
                    while shown < capacity && !failed && book_offset+shown<count {
                        let index=filter.index(book_offset+shown,st.book_count,bits);
                        let n=self.detailed_book_row(x,top+shown as i32*row_h,w,row_h,index,scratch);
                        if n<=0 {failed=n<0;break;}
                        if !self.touch && !self.sleep_render && book_offset+shown==selected{
                            dsn_stroke(x+3,top+shown as i32*row_h,w-6,row_h-2,BLACK,2,5,15);
                        }
                        shown+=1;
                    }
                } else if st.err != ERR_UNAVAILABLE {
                    let mut batch=[EMPTY_LOG_BOOK;2];
                    while shown < capacity && !failed {
                    let count=if st.native_stats {
                        match native_book_batch(book_offset+shown,&mut batch,scratch) {
                            Some(n) => n,
                            None => { failed=true; break; }
                        }
                    } else { log_books_page(book_offset+shown,&mut batch,scratch) };
                    if count==0 {break;}
                    for book in &batch[..count.min((capacity-shown) as usize)] {
                        let yy=top+shown as i32*row_h;
                        let title=if book.len>0 { &book.title[..book.len as usize] } else { b"Untitled" };
                        self.book_row(x,yy,w,row_h,title,book.progress);
                        shown+=1;
                    }

                    }
                } else { failed = true; }
                if shown == 0 {
                    self.centered(x + 12, top + self.lb, w - 24, self.lb,
                                  if failed { b"Reading history unavailable" } else { b"No more books to display" }, false);
                }
                // The headline already conveys the count when all books fit.
                if count > capacity {
                    let first = book_offset + 1;
                    let last = (book_offset + shown).min(count);
                    let end = label!(s(b"Showing "), n(first), s(b"-"), n(last), s(b" of "), n(count), s(b" books"));
                    text(x + 12, y + available - self.ls - 5, w - 24, self.ls,
                         end.get(), SMALL, CENTER, 0);
                }
            }
            _ => {}
        }
    }
}
