// Offline preview of the future cross-device Overall Statistics page.
// Everything shown here is genuine device-local data, never a fabricated server total.

#[derive(Clone, Copy)]
struct CarouselBook {
    path: [u8; 512],
    path_n: usize,
    title: [u8; 192],
    title_n: usize,
    author: [u8; 128],
    author_n: usize,
    series: [u8; 112],
    series_n: usize,
    percent: i32,
}

impl CarouselBook {
    const EMPTY: Self = Self {
        path: [0; 512], path_n: 0,
        title: [0; 192], title_n: 0,
        author: [0; 128], author_n: 0,
        series: [0; 112], series_n: 0, percent: -1,
    };
    fn path(&self) -> &[u8] { &self.path[..self.path_n] }
    fn title(&self) -> &[u8] { &self.title[..self.title_n] }
    fn author(&self) -> &[u8] { &self.author[..self.author_n] }
    fn series(&self) -> &[u8] { &self.series[..self.series_n] }
}

struct BookCarousel {
    books: [CarouselBook; 5],
    count: usize,
    selected: usize,
}

impl BookCarousel {
    fn new() -> Self {
        Self { books: [CarouselBook::EMPTY; 5], count: 0, selected: 0 }
    }

    fn load(&mut self) {
        // One bounded scan during startup. Never reopen the index on redraw.
        let mut shelf = [0u8; 2400];
        let mut index = 0u32;
        while index < 40 && self.count < self.books.len() {
            let bytes = dsn_recent_shelf(index, 8, &mut shelf);
            if bytes <= 0 { break; }
            let bytes = (bytes as usize).min(shelf.len());
            let mut pos = 0usize;
            let mut scanned = 0u32;
            while pos < bytes && self.count < self.books.len() {
                let Some(rel_end) = shelf[pos..bytes].iter().position(|&b| b == b'\n') else { break };
                let end = pos + rel_end;
                let record = &shelf[pos..end];
                pos = end + 1;
                scanned += 1;
                let Some(path_end) = record.iter().position(|&b| b == b'\t') else { continue };
                let title_start = path_end + 1;
                let Some(title_end_rel) = record[title_start..].iter().position(|&b| b == b'\t') else { continue };
                let title_end = title_start + title_end_rel;
                let author_start = title_end + 1;
                let author_end = record[author_start..].iter()
                    .position(|&b| b == b'\t').map(|p| author_start + p).unwrap_or(record.len());
                let path = &record[..path_end];
                if !path.starts_with(b"/") || path.len() > 512 { continue; }
                let pct = dsn_book_progress(path);
                if !(1..100).contains(&pct) { continue; }
                let book = &mut self.books[self.count];
                book.path[..path.len()].copy_from_slice(path);
                book.path_n = path.len();
                let title = &record[title_start..title_end];
                book.title_n = title.len().min(book.title.len());
                book.title[..book.title_n].copy_from_slice(&title[..book.title_n]);
                let author = &record[author_start..author_end];
                book.author_n = author.len().min(book.author.len());
                book.author[..book.author_n].copy_from_slice(&author[..book.author_n]);
                // Shelf rows include a progress field followed by an optional
                // fifth field containing the formatted series name and index.
                if author_end < record.len() {
                    let progress_end = record[author_end + 1..].iter()
                        .position(|&b| b == b'\t').map(|p| author_end + 1 + p);
                    if let Some(series_sep) = progress_end {
                        let series = &record[series_sep + 1..];
                        book.series_n = series.len().min(book.series.len());
                        book.series[..book.series_n].copy_from_slice(&series[..book.series_n]);
                    }
                }
                book.percent = pct;
                self.count += 1;
            }
            if scanned == 0 { break; }
            index += scanned;
        }
    }
    fn step(&mut self, delta: i32) {
        if self.count < 2 { return; }
        self.selected = ((self.selected as i32 + delta)
            .rem_euclid(self.count as i32)) as usize;
    }
    fn current(&self) -> Option<&CarouselBook> {
        if self.count == 0 { None } else { Some(&self.books[self.selected]) }
    }
}

impl Ui {
    fn overall_carousel_bounds(&self) -> (i32, i32) {
        let top = 13 + self.ls + self.lt;
        let available = (self.content_bottom() - top - 13).max(1);
        let gap = 8;
        let hero_h = available * 31 / 100;
        let metrics_h = available * 24 / 100;
        let y = top + hero_h + metrics_h + 2 * gap;
        (y, (top + available - y).max(1))
    }

    fn overall_carousel_contains(&self, x: i32, y: i32) -> bool {
        let (top, height) = self.overall_carousel_bounds();
        x >= G && x < self.w - G && y >= top && y < top + height
    }

    fn overall_month_bounds(&self) -> (i32, i32, i32, i32) {
        let y = 13 + self.ls + self.lt;
        let available = (self.content_bottom() - y - 13).max(1);
        (G, y, self.w - 2 * G, available * 31 / 100)
    }

    // Sync icon stays hidden after a fresh successful sync until the user
    // touches This Month, or the last success ages beyond 24 hours.
    fn overall_refresh_bounds(&self) -> (i32, i32, i32, i32) {
        let top = 13 + self.ls + self.lt;
        (self.w - G - 42, top + 1, 38, 34)
    }

    // Confirmation replaces the Overall viewport but keeps its page index.
    // Touch zones and hardware Confirm/Back are both supported.
    fn overall_sync_confirmation(&self) {
        dsn_clear();
        self.header(0);
        let x = G;
        let w = self.w - 2 * G;
        let y = 13 + self.ls + self.lt;
        let h = (self.content_bottom() - y - 13).max(1);
        self.panel(x, y, w, h);
        let mid = y + h / 2;
        text(x + 10, mid - 2 * self.lt - 18, w - 20, self.lt,
             b"Sync with server?", TITLE, CENTER, BOLD);
        text(x + 10, mid - self.ls - 8, w - 20, self.ls,
             b"Download updated reading totals", SMALL, CENTER, 0);
        let (yes_x, no_x, by, bw, bh) = self.overall_sync_confirmation_buttons();
        dsn_stroke(yes_x, by, bw, bh, BLACK, 2, 7, 15);
        dsn_stroke(no_x, by, bw, bh, BLACK, 1, 7, 15);
        text(yes_x + 4, by + (bh - self.lb) / 2, bw - 8, self.lb,
             b"Sync now", BODY, CENTER, BOLD);
        text(no_x + 4, by + (bh - self.lb) / 2, bw - 8, self.lb,
             b"Cancel", BODY, CENTER, 0);
    }

    fn overall_sync_confirmation_buttons(&self) -> (i32, i32, i32, i32, i32) {
        let x = G;
        let w = self.w - 2 * G;
        let y = 13 + self.ls + self.lt;
        let h = (self.content_bottom() - y - 13).max(1);
        let gap = 12;
        let width = (w - 32 - gap) / 2;
        let bh = (self.lb + 19).max(36);
        let button_y = (y + h / 2 + 12).min(y + h - bh - 10);
        (x + 10, x + w - 10 - width, button_y, width, bh)
    }

    fn overall_preview(&self, st: &Stats, month_day: u32, clock_ok: bool, carousel: &BookCarousel,
                       server: &ServerOverall, paired: bool, syncing: bool) {
        dsn_clear();
        self.header(0);
        let x = G;
        let w = self.w - G * 2;
        let y = 13 + self.ls + self.lt;
        let available = (self.content_bottom() - y - 13).max(1);
        let gap = 8;
        let hero_h = available * 31 / 100;
        let metrics_h = available * 24 / 100;
        let recent_y = y + hero_h + metrics_h + 2 * gap;
        let recent_h = (y + available - recent_y).max(1);

        self.panel(x, y, w, hero_h);
        let headline: &[u8] = if server.available { b"THIS MONTH - SERVER" }
            else { b"THIS MONTH" };
        let stale = server.last_synced == 0 || dsn_time().saturating_sub(server.last_synced) >= 86_400;
        let show_icon = paired && (server.icon_revealed || stale);
        let heading_width = if show_icon { w - 58 } else { w - 16 };
        text(x + 8, y + 6, heading_width, self.ls, headline, SMALL, CENTER, BOLD);
        if show_icon {
            let (bx, by, bw, bh) = self.overall_refresh_bounds();
            let cx = bx + bw / 2;
            let cy = by + bh / 2;
            // Circular sync arrows, pure line geometry: no font glyph or
            // extra Dessen ABI dependency, clearly visible on grayscale e-ink.
            for (ax, ay, ex, ey) in [
                (cx - 8, cy, cx - 6, cy - 6),
                (cx - 6, cy - 6, cx, cy - 9),
                (cx, cy - 9, cx + 7, cy - 7),
                (cx + 7, cy - 7, cx + 9, cy - 3),
                (cx + 8, cy, cx + 6, cy + 6),
                (cx + 6, cy + 6, cx, cy + 9),
                (cx, cy + 9, cx - 7, cy + 7),
                (cx - 7, cy + 7, cx - 9, cy + 3),
                (cx + 9, cy - 3, cx + 3, cy - 4),
                (cx + 9, cy - 3, cx + 11, cy - 9),
                (cx - 9, cy + 3, cx - 3, cy + 4),
                (cx - 9, cy + 3, cx - 11, cy + 9),
            ] {
                dsn_line(ax, ay, ex, ey, 2, BLACK);
            }
        }
        let mut month_secs = 0u64;
        let mut active = 0u32;
        if clock_ok {
            for ago in 0..month_day.min(DAYS as u32) {
                let seconds = st.day[DAYS - 1 - ago as usize];
                month_secs += seconds as u64;
                if seconds > 0 { active += 1; }
            }
        }
        if server.available { month_secs = server.month_seconds as u64; active = server.month_active_days; }
        let month = if syncing { S::new().s(b"Syncing...")
        } else if clock_ok || server.available {
            dur(month_secs.min(u32::MAX as u64) as u32)
        } else { S::new().s(b"--") };
        let big_h = dsn_line_height(LARGE);
        text(x + 8, y + self.ls + 12, w - 16, big_h, month.get(), LARGE, CENTER, BOLD);
        let line_y = y + hero_h * 56 / 100;
        dsn_line(x + 8, line_y, x + w - 8, line_y, 1, DARK);
        let values = [
            S::new().n(if server.available { server.current_streak } else { st.streaks().0 }),
            if clock_ok || server.available { S::new().n(active) } else { S::new().s(b"--") },
            S::new().n(if server.available { server.books } else { st.book_count }),
        ];
        let labels: [&[u8]; 3] = [b"Day Streak", b"Active Days", b"Books"];
        for i in 0..3 {
            let cx = x + (w * (2 * i + 1)) / 6;
            if i > 0 { dsn_line(x + w * i / 3, line_y + 5, x + w * i / 3, y + hero_h - 7, 1, DARK); }
            let cw = w / 3 - 6;
            let font = metric_font(values[i as usize].get(), TITLE, BODY, cw);
            text(cx - cw / 2, line_y + 7, cw, dsn_line_height(font),
                values[i as usize].get(), font, CENTER, BOLD);
            text(cx - cw / 2, line_y + 9 + self.lt, cw, self.ls,
                labels[i as usize], SMALL, CENTER, 0);
        }

        let mw = (w - gap) / 2;
        let mh = (metrics_h - gap) / 2;
        self.metric(x, y + hero_h + gap, mw, mh, b"Sessions",
            S::new().n(if server.available { server.sessions } else { st.sessions }).get(), 0);
        self.metric(x + mw + gap, y + hero_h + gap, w - mw - gap, mh,
            b"Page Turns", S::new().n(if server.available { server.page_turns } else { st.progress_bp }).get(), 1);
        // Two distinct statistics from retained device history (up to 371
        // days). Avoid repeating the month's time and book count above.
        let (best_day, daily_average, active_days) = st.daily_summary();
        let best = if server.available { server.best_day_seconds } else { best_day };
        let avg = if server.available { server.daily_average_seconds } else { daily_average };
        let have_days = if server.available { server.month_active_days > 0 } else { active_days > 0 };
        let best_label = if have_days { dur(best) } else { S::new().s(b"--") };
        let avg_label = if have_days { dur(avg) } else { S::new().s(b"--") };
        self.metric(x, y + hero_h + gap + mh + gap, mw, mh,
            b"Best Day", best_label.get(), 3);
        self.metric(x + mw + gap, y + hero_h + gap + mh + gap, w - mw - gap, mh,
            b"Daily Avg.", avg_label.get(), 2);

        self.panel(x, recent_y, w, recent_h);
        let counter_w = if carousel.count > 1 { 56 } else { 0 };
        text(x + 9, recent_y + 7, w - 18, self.lb, b"Continue Reading", BODY, CENTER, BOLD);
        if carousel.count > 1 {
            let indicator = S::new().n((carousel.selected + 1) as u32)
                .s(b"/").n(carousel.count as u32);
            text(x + w - counter_w - 11, recent_y + 7, counter_w, self.lb,
                indicator.get(), SMALL, RIGHT, 0);
        }
        dsn_line(x + 9, recent_y + self.lb + 11, x + w - 9, recent_y + self.lb + 11, 1, DARK);
        let cards_y = recent_y + self.lb + 16;
        let cards_h = (recent_h - self.lb - 23).max(1);
        if let Some(book) = carousel.current() {
            // The counter is in the header, leaving the full card height for
            // the portrait cover and two-line book metadata.
            let content_h = cards_h;
            let cover_limit = (content_h - 20).min(200).min((w / 3 - 15) * 4 / 3).max(16);
            let mut cover = [0u8; 4096];
            let bytes = dsn_book_cover(book.path(), cover_limit, &mut cover);
            let cover_len = if bytes > 4 { (bytes as usize).min(cover.len()) } else { 0 };
            self.book_hero(x, cards_y, w, content_h, &cover[..cover_len],
                book.title(), book.author(), book.series(), book.percent);
        } else {
            text(x + 12, cards_y + cards_h / 2, w - 24, self.ls,
                b"No unfinished recent books", SMALL, CENTER, 0);
        }
    }
}