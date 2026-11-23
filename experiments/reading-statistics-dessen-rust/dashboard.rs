// Photo-matched two-page dashboard for the Dessen Rust statistics experiment.
// All statistics come from validated native records; writes are deliberately absent.
const HEADER_H: i32 = 42;

struct Current {
    found: bool,
    secs: u32,
    sessions: u32,
    turns: u32,
    active: u32,
    periods: [u32; 4],
}

fn load_current(home: &Home) -> Current {
    let mut out = Current { found: false, secs: 0, sessions: 0, turns: 0, active: 0, periods: [0; 4] };
    if !home.has_book() { return out; }
    let mut record = [0u8; 316];
    for i in 0..2048u32 {
        if dsn_fs_read_at(BOOK_INDEX, i * 316, &mut record) != 316 { break; }
        if !record_valid(&record, 0x314b4252, 1) { break; }
        let path_end = record[16..192].iter().position(|&b| b == 0).unwrap_or(176);
        if &record[16..16 + path_end] != home.field(0) { continue; }
        let hash = (le32(&record, 12) as u64) << 32 | le32(&record, 8) as u64;
        let mut path = [0u8; 80];
        const HEX: &[u8; 16] = b"0123456789abcdef";
        let make_path = |p: &mut [u8; 80], prefix: &[u8], suffix: &[u8]| {
            p[..prefix.len()].copy_from_slice(prefix);
            for j in 0..16 {
                p[prefix.len() + j] = HEX[((hash >> (60 - j * 4)) & 15) as usize];
            }
            let end = prefix.len() + 16;
            p[end..end + suffix.len()].copy_from_slice(suffix);
            end + suffix.len()
        };
        let n = make_path(&mut path, BOOK_PREFIX, BOOK_SUFFIX);
        let mut snap = [0u8; 1500];
        if !read_native(&path[..n], &mut snap, 0x32545352, 2) {
            let n = make_path(&mut path, BOOK_PREFIX, BOOK_SUFFIX_BACKUP);
            if !read_native(&path[..n], &mut snap, 0x32545352, 2) { break; }
        }
        out.found = true;
        out.secs = (((le32(&snap, 20) as u64) << 32) | le32(&snap, 16) as u64).min(u32::MAX as u64) as u32;
        out.sessions = le32(&snap, 12);
        out.turns = le32(&snap, 24);
        let latest = le32(&snap, 28);
        for ago in 0..366 {
            if latest <= ago { break; }
            let ordinal = latest - ago;
            if le32(&snap, 32 + (ordinal as usize % 366) * 4) != 0 { out.active += 1; }
        }
        let prefix = b"/.crosspoint/reading-stats/time_";
        let suffix = b"_v1.bin";
        let n = make_path(&mut path, prefix, suffix);
        let mut times = [0u8; 148];
        if read_native(&path[..n], &mut times, 0x31505452, 1) {
            for j in 0..4 { out.periods[j] = le32(&times, 16 + 4 * j); }
        } else {
            let suffix = b"_v1.bin.bak";
            let n = make_path(&mut path, prefix, suffix);
            if read_native(&path[..n], &mut times, 0x31505452, 1) {
                for j in 0..4 { out.periods[j] = le32(&times, 16 + 4 * j); }
            }
        }
        break;
    }
    out
}

impl Ui {
    fn panel(&self, x: i32, y: i32, w: i32, h: i32) {
        dsn_stroke(x, y, w, h, BLACK, 1, 10, 15);
    }

    fn centered(&self, x: i32, y: i32, w: i32, h: i32, s: &[u8], bold: bool) {
        text(x, y, w, h, s, BODY, CENTER, if bold { BOLD } else { 0 });
    }

    fn metric(&self, x: i32, y: i32, w: i32, h: i32, label: &[u8], value: &[u8], icon: i32) {
        self.panel(x, y, w, h);
        // Preserve space for Noto Sans 12 labels on narrow X3-sized cards.
        let iconw = (w / 4).clamp(30, 54);
        let cy = y + h / 2;
        let ix = x + iconw / 2;
        // Small native vector icons: stacked pages, page turn, calendar, speed dial.
        match icon {
            0 => {
                for offset in [0, 4, 8] {
                    dsn_stroke(ix - 10 + offset, cy - 13 + offset, 22, 24, BLACK, 1, 0, 0);
                }
            }
            1 => {
                dsn_stroke(ix - 9, cy - 15, 24, 30, BLACK, 1, 0, 0);
                for d in [0, 6, 12] { dsn_line(ix - 5, cy - 8 + d, ix + 9, cy - 8 + d, 1, BLACK); }
            }
            2 => {
                dsn_stroke(ix - 12, cy - 12, 26, 26, BLACK, 1, 0, 0);
                dsn_line(ix - 12, cy - 4, ix + 14, cy - 4, 1, BLACK);
            }
            _ => {
                dsn_stroke(ix - 13, cy - 12, 27, 26, BLACK, 1, 13, 15);
                dsn_line(ix, cy + 2, ix + 10, cy - 7, 2, BLACK);
            }
        }
        dsn_line(x + iconw, y + 7, x + iconw, y + h - 7, 1, DARK);
        let tx = x + iconw + 4;
        let tw = w - iconw - 8;
        let preferred = metric_font(value, TITLE, BODY, tw);
        // Short metric rows need smaller values to keep both text lines
        // within the card, with a visible bottom margin.
        let font = if h < self.ls + dsn_line_height(preferred) + 14 {
            BODY
        } else { preferred };
        let value_h = dsn_line_height(font);
        let gap = ((h - self.ls - value_h - 12) / 2).clamp(3, 8);
        let label_y = y + ((h - self.ls - value_h - gap) / 2).max(3);
        let value_y = (label_y + self.ls + gap).min(y + h - value_h - 5);
        text(tx, label_y, tw, self.ls, label, SMALL, CENTER, BOLD);
        text(tx, value_y, tw, value_h, value, font, CENTER, BOLD);
    }

    fn periods_min_height(&self) -> i32 {
        self.lb + 19 + 4 * (self.ls + 2)
    }

    // Shared, font-measured Time of Day widget for Overall, Current Book,
    // and Your Week. The bars never encroach on the label column.
    fn periods(&self, x: i32, y: i32, w: i32, h: i32, values: &[u32; 4]) {
        self.panel(x, y, w, h);
        self.centered(x, y + 5, w, self.lb, b"Time of Day", true);
        dsn_line(x + 10, y + self.lb + 8, x + w - 10, y + self.lb + 8, 1, DARK);
        let names: [&[u8]; 4] = [b"Morning", b"Afternoon", b"Evening", b"Night"];
        let label_text_w = names.iter()
            .map(|s| (dsn_measure_text(s, SMALL, 0) >> 16) as i32)
            .max().unwrap_or(0);
        let label_x = x + 10;
        // Keep a right-hand percentage column clear of the bar ends.
        // The Picco's real Noto Sans glyphs exceed a hard-coded 48px box.
        // Reserve room for the widest possible percentage, including padding.
        let pct_w = (((dsn_measure_text(b"100%", SMALL, 0) >> 16) as i32) + 12).max(60);
        let pct_x = x + w - pct_w - 10;
        let bar_x = (label_x + label_text_w + 12).min(pct_x - 26);
        let bar_w = (pct_x - bar_x - 10).max(1);
        let top = y + self.lb + 13;
        let row_h = ((y + h - 6 - top) / 4).max(1);
        let max = values.iter().copied().max().unwrap_or(0).max(1);
        let total: u64 = values.iter().map(|v| *v as u64).sum();
        for i in 0..4 {
            let row_y = top + i as i32 * row_h;
            let mid_y = row_y + row_h / 2;
            let text_y = mid_y - self.ls / 2;
            text(label_x, text_y, (bar_x - label_x - 6).max(1), self.ls,
                names[i], SMALL, LEFT, 0);
            let fill = (values[i] as u64 * bar_w as u64 / max as u64) as i32;
            if fill > 0 {
                dsn_fill(bar_x, mid_y - 3, fill.max(2).min(bar_w), 6, BLACK);
            }
            let pct = if total > 0 { (values[i] as u64 * 100 + total / 2) / total } else { 0 };
            let label = S::new().n(pct as u32).s(b"%");
            text(pct_x, text_y, pct_w, self.ls, label.get(), SMALL, RIGHT, 0);
        }
    }

    fn week_chart(&self, x: i32, y: i32, w: i32, h: i32, st: &Stats) {
        self.panel(x, y, w, h);
        self.centered(x, y + 5, w, self.lb, b"Latest 7 Days", false);
        let top = y + self.lb + 13;
        let base = y + h - self.ls - 7;
        let chart_h = (base - top).max(1);
        let cw = (w - 18) / 7;
        let week = &st.day[DAYS - 7..];
        let max = week.iter().copied().max().unwrap_or(0).max(1);
        let wd = (st.end + 4) % 7; // Sunday = 0
        for (i, &v) in week.iter().enumerate() {
            let xx = x + 9 + i as i32 * cw;
            let bar = (v as u64 * chart_h as u64 / max as u64) as i32;
            if bar > 0 { dsn_fill(xx + cw / 4, base - bar, (cw / 2).max(2), bar.max(2), BLACK); }
            let day = ((wd as usize + 1 + i) % 7) * 3;
            text(xx, base + 3, cw, self.ls, &b"SunMonTueWedThuFriSat"[day..day + 3], SMALL, CENTER, 0);
        }
        dsn_line(x + 10, base, x + w - 10, base, 1, DARK);
    }

    fn header(&self, page: i32) {
        let g = G;
        let mut c = [0u8; 8];
        if dsn_clock(&mut c) >= 0 && (c[0] | c[1]) != 0 {
            let hour = if c[4] % 12 == 0 { 12 } else { c[4] % 12 };
            let s = S::new().n(hour as u32).s(b":").n(c[5] as u32 / 10).n(c[5] as u32 % 10);
            text(g, 5, self.w / 3, self.ls, s.get(), SMALL, LEFT, 0);
        }
        let battery = dsn_battery();
        if battery >= 0 {
            text(self.w - g - self.w / 4, 5, self.w / 4, self.ls,
                S::new().n(battery as u32 & 255).s(b"%").get(), SMALL, RIGHT, 0);
        }
        let title: &[u8] = match page {
            0 => b"Overall Statistics",
            1 => b"Device Statistics",
            2 => b"Current Book",
            3 => b"Your Week",
            4 => b"Reading Calendar",
            _ => b"All Books",
        };
        if self.touch {
            let arrow_y = 5 + self.ls + self.lt / 2;
            dsn_line(g + 7, arrow_y, g + 29, arrow_y, 2, BLACK);
            dsn_line(g + 7, arrow_y, g + 17, arrow_y - 9, 2, BLACK);
            dsn_line(g + 7, arrow_y, g + 17, arrow_y + 9, 2, BLACK);
        }
        text(g, 5 + self.ls, self.w - 2 * g, self.lt, title, TITLE, CENTER, BOLD);
        dsn_line(g, 7 + self.ls + self.lt, self.w - g, 7 + self.ls + self.lt, 1, DARK);
    }

    fn dashboard_render(&self, st: &Stats, home: &Home, detail: &Current, page: i32, month_day: u32, clock_ok: bool) {
        dsn_clear();
        self.header(page + 1);
        let x = G;
        let w = self.w - G * 2;
        let gap = 7;
        let top = 13 + self.ls + self.lt;
        let available = (self.content_bottom() - top - 12 - gap * 3).max(1);
        if page == 0 {
            // The monthly hero is independent of an active or recent book.
            let hero = available * 31 / 100;
            let metric_h = available * 18 / 100;
            let periods_h = (available * 25 / 100)
                .max(self.periods_min_height())
                .min((available - hero - metric_h - self.ls - 22).max(1));
            let week_h = available - hero - metric_h - periods_h;
            self.panel(x, top, w, hero);
            self.centered(x, top + 5, w, self.lb, b"THIS MONTH", true);
            let mut month_secs = 0u64;
            let mut active = 0u32;
            if clock_ok {
                for ago in 0..month_day.min(DAYS as u32) {
                    let v = st.day[DAYS - 1 - ago as usize];
                    month_secs += v as u64;
                    if v > 0 { active += 1; }
                }
            }
            let value = if clock_ok { dur(month_secs.min(u32::MAX as u64) as u32) } else { S::new().s(b"--") };
            // Use Dessen's native FreeInk font. The hand-drawn 5x7 digits
            // became pixelated at the enlargement used on X3/Picco.
            let headline_top = top + self.lb + 13;
            let large_h = dsn_line_height(LARGE).max(self.lt);
            let headline_h = (hero * 56 / 100 - self.lb - 20).max(large_h);
            text(x + 6, headline_top + (headline_h - large_h) / 2,
                w - 12, large_h, value.get(), LARGE, CENTER, BOLD);
            let line = top + hero * 56 / 100;
            dsn_line(x + 9, line, x + w - 9, line, 1, DARK);
            for i in 1..3 { dsn_line(x + w * i / 3, line + 6, x + w * i / 3, top + hero - 8, 1, DARK); }
            let streak = st.streaks().0;
            let vals = [S::new().n(streak), if clock_ok { S::new().n(active) } else { S::new().s(b"--") }, S::new().n(st.book_count)];
            let labels: [&[u8]; 3] = [b"Day Streak", b"Active Days", b"Books"];
            let bottom_space = (top + hero - 8 - line).max(self.lt + self.ls);
            let values_top = line + ((bottom_space - self.lt - self.ls) / 2).max(4);
            for i in 0..3 {
                let left = x + w * i / 3;
                let right = x + w * (i + 1) / 3;
                let ww = right - left;
                let v = vals[i as usize].get();
                let font = metric_font(v, TITLE, BODY, ww - 4);
                text(left, values_top, ww, dsn_line_height(font), v, font, CENTER, BOLD);
                text(left, values_top + self.lt, ww, self.ls, labels[i as usize], SMALL, CENTER, 0);
            }
            let my = top + hero + gap;
            let mw = (w - gap) / 2;
            self.metric(x, my, mw, metric_h, b"Sessions", S::new().n(st.sessions).get(), 0);
            self.metric(x + mw + gap, my, w - mw - gap, metric_h, b"Page Turns", S::new().n(st.progress_bp).get(), 1);
            let py = my + metric_h + gap;
            let vals = [st.hour[0], st.hour[1], st.hour[2], st.hour[3]];
            self.periods(x, py, w, periods_h, &vals);
            self.week_chart(x, py + periods_h + gap, w, week_h, st);
        } else {
            let mut metrics = available * 38 / 100;
            let min_metrics = 2 * (self.ls + self.lb + 8) + gap;
            let periods_h = (available * 27 / 100)
                .max(self.periods_min_height())
                .min((available - min_metrics - self.book_hero_min_height()).max(1));
            // Current Book must reserve the same two-line title/series space
            // as Continue Reading. Borrow height from the metric cards first.
            if available - metrics - periods_h < self.book_hero_min_height() {
                metrics = (available - periods_h - self.book_hero_min_height()).max(min_metrics);
            }
            let hero = available - metrics - periods_h;
            self.panel(x, top, w, hero);
            if home.has_book() {
                self.book_hero(x, top, w, hero, &home.cover[..home.cover_n],
                    home.field(1), home.field(2), home.field(4), home.percent);
            } else {
                self.centered(x + 10, top + hero / 3, w - 20, self.lb, b"No recent book", false);
            }
            let my = top + hero + gap;
            let mw = (w - gap) / 2;
            let mh = (metrics - gap) / 2;
            let sessions = if detail.found { S::new().n(detail.sessions) } else { S::new().s(b"--") };
            let turns = if detail.found { S::new().n(detail.turns) } else { S::new().s(b"--") };
            let active = if detail.found { S::new().n(detail.active) } else { S::new().s(b"--") };
            let avg = if detail.found && detail.turns > 0 {
                S::new().n(detail.secs / detail.turns).s(b"s")
            } else { S::new().s(b"--") };
            self.metric(x, my, mw, mh, b"Sessions", sessions.get(), 0);
            self.metric(x + mw + gap, my, w - mw - gap, mh, b"Page Turns", turns.get(), 1);
            self.metric(x, my + mh + gap, mw, mh, b"Active Days", active.get(), 2);
            self.metric(x + mw + gap, my + mh + gap, w - mw - gap, mh, b"Avg. / Turn", avg.get(), 3);
            self.periods(x, my + metrics + gap, w, periods_h, &detail.periods);
        }
        if st.sessions == 0 && page == 0 {
            let msg: &[u8] = if st.err == ERR_UNAVAILABLE { b"Reading stats need newer firmware." }
                else { b"No reading sessions yet. Read a book and come back." };
            dsn_text_box(x + 10, top + self.lt + self.lb + 8, w - 20, self.lb * 2,
                msg, BODY, CENTER, 2, BOLD);
        }
    }
}
