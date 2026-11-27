// Calendar visual parity with the native Razor-Crest Reading Statistics plugin.
// The date grid is Monday-first, with a bounded four-level gray activity scale.
impl Ui {
    fn calendar_bounds(&self) -> (i32,i32,i32,i32) {
        let y=13+self.ls+self.lt;
        let available=(self.content_bottom()-y-13).max(1);
        // Header, weekdays, six complete rows, legend and clearance.
        let minimum=self.lb+9*self.ls+52;
        let insights_min=3*self.lb+self.lt+2*self.ls+46;
        let h=(available*68/100).max(minimum)
            .min((available-insights_min).max(minimum)).min(available);
        (G,y,self.w-2*G,h)
    }
    #[inline(never)]
    fn calendar_page(&self, st: &Stats, year: u32, month: u32, today: u32, all:bool, data:&[u8],local_days:&[u8]) {
        let x = G;
        let w = self.w - 2 * G;
        let y = 13 + self.ls + self.lt;
        let available = (self.content_bottom() - y - 13).max(1);
        if year < 1970 || !(1..=12).contains(&month) {
            self.panel(x, y, w, available);
            text(x + 10, y + self.lb, w - 20, self.lb * 2,
                b"No dated reading sessions or device time.", BODY, CENTER, 0);
            return;
        }
        let gap = 9;
        // Reserve the full six-week grid before distributing the remaining height.
        let cal_h = self.calendar_bounds().3;
        let insights_y = y + cal_h + gap;
        let insights_h = (available - cal_h - gap).max(0);
        self.panel(x, y, w, cal_h);

        static MONTHS: [&[u8]; 12] = [b"January", b"February", b"March", b"April",
            b"May", b"June", b"July", b"August", b"September", b"October",
            b"November", b"December"];
        static SHORT_MONTHS: [&[u8]; 12] = [b"Jan", b"Feb", b"Mar", b"Apr",
            b"May", b"Jun", b"Jul", b"Aug", b"Sep", b"Oct", b"Nov", b"Dec"];
        let month_title = label!(s(MONTHS[month as usize - 1]), s(b" "), n(year));
        let heading_h = self.lb;
        // Reserve measured width for navigation on narrow calendar cards.
        let nav_w = if self.touch || self.sleep_render {0}else{
            (((dsn_measure_text(b"Next >", SMALL, BOLD) >> 16) as i32) + 12)
            .max(((dsn_measure_text(b"< Prev", SMALL, BOLD) >> 16) as i32) + 12)
            .min(w / 3)
        };
        text(x + nav_w + 8, y + 6, (w - 2 * nav_w - 16).max(1), heading_h,
            month_title.get(), BODY, CENTER, BOLD);
        if !self.touch && !self.sleep_render {
            text(x + 8, y + 7, nav_w, heading_h, b"< Prev", SMALL, LEFT, BOLD);
            text(x + w - nav_w - 8, y + 7, nav_w, heading_h,b"Next >", SMALL, RIGHT, BOLD);
        }
        let source: &[u8]=if !data.is_empty(){if calendar_owned(st,data){b"All Devices".as_slice()}else{b"All Devices (server only)"}}else if all{b"This Device (not synced)"}else{b"This Device"};
        text(x+8,y+heading_h+5,w-16,self.ls,source,SMALL,CENTER,0);
        let divider_y = y + heading_h + self.ls + 11;
        dsn_line(x + 9, divider_y, x + w - 9, divider_y, 1, DARK);
        let weekday_top = divider_y + 5;
        let first = days_from_civil(year, month, 1);
        let count = days_in_month(year, month);
        // Local ordinal: 1970-01-01 = Thursday. Monday is column 0.
        let offset = (first + 3) % 7;
        let rows = ((offset + count + 6) / 7) as i32;
        let cell_gap = 3;
        let cell_w = ((w - 24 - 6 * cell_gap) / 7).max(1);
        let legend_y = y + cal_h - self.ls - 7;
        let grid_top = weekday_top + self.ls + 3;
        let room = (legend_y - grid_top - 8 - (rows - 1) * cell_gap).max(rows);
        let cell_h = cell_w.min(room / rows).max(1);
        let grid_w = 7 * cell_w + 6 * cell_gap;
        let grid_x = x + (w - grid_w) / 2;
        static LABELS: [&[u8]; 7] = [b"Mo", b"Tu", b"We", b"Th", b"Fr", b"Sa", b"Su"];
        for col in 0..7 {
            let xx = grid_x + col * (cell_w + cell_gap);
            text(xx, weekday_top, cell_w, self.ls,
                LABELS[col as usize], SMALL, CENTER, BOLD);
        }

        let elapsed = if today != 0 { today.min(count) } else { count };
        let mut active = 0u32;
        let mut total = 0u64;
        let mut best_seconds = 0u32;
        let mut best_day = 0u32;
        for date in 1..=count {
            let index = offset + date - 1;
            let xx = grid_x + (index % 7) as i32 * (cell_w + cell_gap);
            let yy = grid_top + (index / 7) as i32 * (cell_h + cell_gap);
            let ordinal = days_from_civil(year, month, date);
            let seconds=if date<=elapsed && !local_days.is_empty(){le32(local_days,(date-1) as usize*4)}else{0};
            let seconds=if date<=elapsed {calendar_seconds(st,data,ordinal,seconds)}else{0};
            if seconds > 0 {
                active += 1;
                total += seconds as u64;
                if seconds > best_seconds { best_seconds = seconds; best_day = date; }
            }
            // Native thresholds: none, <15 minutes, 15â€“44 minutes, 45+ minutes.
            let intensity = if seconds >= 2700 { 3 } else if seconds >= 900 { 2 }
                else if seconds > 0 { 1 } else { 0 };
            let shade = match intensity { 1 => LIGHT, 2 => DARK, 3 => BLACK, _ => 0 };
            if shade != 0 && cell_w > 6 && cell_h > 6 {
                dsn_fill(xx + 2, yy + 2, cell_w - 4, cell_h - 4, shade);
            }
            // Outlined numbered cells even when no reading was recorded.
            dsn_stroke(xx, yy, cell_w, cell_h, BLACK,
                if date == today { 2 } else { 1 }, 0, 0);
            text(xx + 1, yy + (cell_h - self.ls) / 2,
                cell_w - 2, self.ls, label!(n(date)).get(),
                SMALL, CENTER, if intensity >= 2 { 2 } else { BOLD });
        }
        static LEGEND: [&[u8]; 4] = [b"None", b"<15m", b"15-44m", b"45m+"];
        let group_w = (w - 16) / 4;
        for i in 0..4 {
            let xx = x + 8 + i * group_w;
            let box_y = legend_y + (self.ls - 12) / 2;
            if i != 0 { dsn_fill(xx, box_y, 12, 12,
                match i { 1 => LIGHT, 2 => DARK, _ => BLACK }); }
            dsn_stroke(xx, box_y, 12, 12, BLACK, 1, 0, 0);
            text(xx + 15, legend_y, group_w - 16, self.ls,
                LEGEND[i as usize], SMALL, LEFT, 0);
        }

        let top_h = (insights_h * 45 / 100).max(self.lb * 2 + self.lt + 12);
        let bottom_y = insights_y + top_h + gap;
        let bottom_h = (insights_h - top_h - gap).max(0);
        let left_w = (w - gap) / 2;
        let right_x = x + left_w + gap;
        let right_w = w - left_w - gap;
        self.panel(x, insights_y, left_w, top_h);
        self.panel(right_x, insights_y, right_w, top_h);
        text(x + 4, insights_y + 7, left_w - 8, self.lb,
            b"Best Day", BODY, CENTER, BOLD);
        text(right_x + 4, insights_y + 7, right_w - 8, self.lb,
            b"Daily Average", BODY, CENTER, BOLD);
        let value_y = insights_y + 10 + self.lb;
        let best_value = if best_day != 0 { dur(best_seconds) } else { label!(s(b"--")) };
        let avg_value = if active != 0 { dur((total / active as u64).min(u32::MAX as u64) as u32) }
            else { label!(s(b"--")) };
        let best_font = metric_font(best_value.get(), TITLE, BODY, left_w - 8);
        let avg_font = metric_font(avg_value.get(), TITLE, BODY, right_w - 8);
        text(x + 4, value_y, left_w - 8, self.lt,
            best_value.get(), best_font, CENTER, BOLD);
        text(right_x + 4, value_y, right_w - 8, self.lt,
            avg_value.get(), avg_font, CENTER, BOLD);
        let caption_y = value_y + self.lt + 3;
        if caption_y + self.ls < insights_y + top_h - 4 {
            let date = if best_day != 0 {
                label!(s(SHORT_MONTHS[month as usize - 1]), s(b" "), n(best_day))
            } else { label!(s(b"No reading yet")) };
            text(x + 4, caption_y, left_w - 8, self.ls,
                date.get(), SMALL, CENTER, 0);
            text(right_x + 4, caption_y, right_w - 8, self.ls,
                b"Per active day", SMALL, CENTER, 0);
        }

        if bottom_h >= self.lb * 2 + 25 {
            self.panel(x, bottom_y, w, bottom_h);
            let consistency = (active * 100 + elapsed / 2) / elapsed.max(1);
            text(x + 12, bottom_y + 9, w * 3 / 4, self.lb,
                b"Reading Consistency", BODY, LEFT, BOLD);
            text(x + w * 3 / 4, bottom_y + 9, w / 4 - 12, self.lt,
                label!(n(consistency), s(b"%")).get(), TITLE, RIGHT, BOLD);
            let bar_y = bottom_y + self.lb + 17;
            let bar_w = w - 24;
            dsn_stroke(x + 12, bar_y, bar_w, 10, BLACK, 1, 0, 0);
            if consistency > 0 {
                dsn_fill(x + 14, bar_y + 2,
                    (bar_w - 4) * consistency as i32 / 100, 6, BLACK);
            }
            text(x + 10, bar_y + 14, w - 20, self.ls,
                label!(n(active), s(b" of "), n(elapsed), s(b" days read")).get(),
                SMALL, CENTER, 0);
        }
    }
}

#[inline(never)]
fn calendar_seconds(st:&Stats,data:&[u8],day:u32,local:u32)->u32 {
    if data.is_empty(){return local;}
    let anchor=le32(data,4);
    let owned=calendar_owned(st,data);
    if day>anchor {return if owned{local}else{0};}
    if anchor-day>=DAYS as u32{return 0;}
    let p=28+(anchor-day) as usize*6;
    let others=le32(data,p)&0xffffff;
    let own=le32(data,p+2)>>8;
    others.saturating_add(if owned{own.max(local)}else{own})
}
#[inline(never)]
fn calendar_owned(st:&Stats,data:&[u8])->bool {
    st.owner!=[0;16] && le32(data,8)==1 &&
        (data[12..28]==st.owner || data[12..28]==[0;16])
}