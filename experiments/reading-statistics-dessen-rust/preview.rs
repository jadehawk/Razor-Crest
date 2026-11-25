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
        series: [0; 112], series_n: 0, percent: 0,
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
    const fn new() -> Self {
        Self { books: [CarouselBook::EMPTY; 5], count: 0, selected: 0 }
    }

    #[inline(never)]
    fn load(&mut self) {
        // One bounded scan during startup. Never reopen the index on redraw.
        let mut shelf = [0u8; 1100];
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
        let (yes_x, no_x, by, yes_w, no_w, bh) = self.overall_sync_confirmation_buttons(b"Sync now");
        dsn_stroke(yes_x, by, yes_w, bh, BLACK, 2, 7, 15);
        dsn_stroke(no_x, by, no_w, bh, BLACK, 1, 7, 15);
        text(yes_x + 4, by + (bh - self.lb) / 2, yes_w - 8, self.lb,
             b"Sync now", BODY, CENTER, BOLD);
        text(no_x + 4, by + (bh - self.lb) / 2, no_w - 8, self.lb,
             b"Cancel", BODY, CENTER, 0);
    }

    fn overall_sync_confirmation_buttons(&self, yes: &[u8]) -> (i32, i32, i32, i32, i32, i32) {
        let x = G;
        let w = self.w - 2 * G;
        let y = 13 + self.ls + self.lt;
        let h = (self.content_bottom() - y - 13).max(1);
        let gap = if self.touch { 28 } else { 12 };
        let room = w - 20 - gap;
        let yes_need = ((dsn_measure_text(yes, BODY, BOLD) >> 16) as i32 + 24).max(64);
        let no_need = ((dsn_measure_text(b"Cancel", BODY, BOLD) >> 16) as i32 + 24).max(64);
        let yes_w = yes_need.min(room - no_need.min(room / 2));
        let no_w = no_need.min(room - yes_w);
        // Keep independently measured buttons centered in their own half.
        // When either needs more than half, use the shared available space.
        let half = (room / 2).max(1);
        let (left, right) = if yes_w <= half && no_w <= half {
            (x + w / 4 - yes_w / 2, x + 3 * w / 4 - no_w / 2)
        } else {
            let left = x + (w - yes_w - no_w - gap) / 2;
            (left, left + yes_w + gap)
        };
        let bh = if self.touch { 62 } else { (self.lb + 19).max(36) };
        let button_y = (y + h / 2 + 12).min(y + h - bh - 10);
        (left, right, button_y, yes_w, no_w, bh)
    }

    #[inline(never)]
    fn overall_preview(&self, carousel: &BookCarousel,
                       server: &ServerOverall, paired: bool, syncing: bool, cover_scratch: &mut [u8; 4096]) {
        dsn_clear();
        self.header(0);
        checkpoint(210);
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
        let stale = server.stale(dsn_time());
        let show_icon = paired && (server.icon_revealed || stale);
        let half = w / 2;
        text(x + 8, y + 6, half - 16, self.ls, b"THIS MONTH", SMALL, CENTER, BOLD);
        text(x + half + 8, y + 6, half - if show_icon { 54 } else { 16 }, self.ls, b"ALL TIME", SMALL, CENTER, BOLD);
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
        checkpoint(211);
        let metric = |value: S| {
            if server.available { value } else { S::new().s(b"N/A") }
        };
        let month = if syncing { S::new().s(b"Syncing...") }
                    else { metric(dur(server.month_seconds)) };
        let big_h = dsn_line_height(LARGE);
        let total = server.total_seconds;
        let all_time = metric(S::new().n(total / 3600).s(b"h ").n(total / 60 % 60).s(b"m"));
        let value_y = y + self.ls + 12;
        let value_w = half - 16;
        let month_font = metric_font(month.get(), LARGE, TITLE, value_w);
        let all_font = metric_font(all_time.get(), LARGE, TITLE, value_w);
        text(x + 8, value_y, value_w, big_h, month.get(), month_font, CENTER, BOLD);
        text(x + half + 8, value_y, value_w, big_h, all_time.get(), all_font, CENTER, BOLD);
        dsn_line(x + half, y + 6, x + half, y + hero_h * 56 / 100 - 4, 1, DARK);
        let line_y = y + hero_h * 56 / 100;
        dsn_line(x + 8, line_y, x + w - 8, line_y, 1, DARK);
        checkpoint(212);
        let values = [
            metric(S::new().n(server.current_streak)),
            metric(S::new().n(server.month_active_days)),
            metric(S::new().n(server.books)),
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

        checkpoint(213);
        let mw = (w - gap) / 2;
        let mh = (metrics_h - gap) / 2;
        self.metric(x, y + hero_h + gap, mw, mh, b"Sessions",
            metric(S::new().n(server.sessions)).get(), 0);
        self.metric(x + mw + gap, y + hero_h + gap, w - mw - gap, mh,
            b"Avg. Session", metric(average_session(server.total_seconds, server.sessions)).get(), 3);
        let best_label = metric(dur(server.best_day_seconds));
        let avg_label = metric(dur(server.daily_average_seconds));
        self.metric(x, y + hero_h + gap + mh + gap, mw, mh,
            b"Best Day", best_label.get(), 3);
        self.metric(x + mw + gap, y + hero_h + gap + mh + gap, w - mw - gap, mh,
            b"Daily Avg.", avg_label.get(), 2);

        checkpoint(214);
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
        checkpoint(215);
        if let Some(book) = carousel.current() {
            // The counter is in the header, leaving the full card height for
            // the portrait cover and two-line book metadata.
            let content_h = cards_h;
            let cover_limit = (content_h - 20).min(200).min((w / 3 - 15) * 4 / 3).max(16);
            // The VM has a fixed 80 KiB budget; use the existing current-book
            // scratch buffer instead of another 4 KiB stack allocation.
            let cover = cover_scratch;
            checkpoint(216);
            let bytes = dsn_book_cover(book.path(), cover_limit, cover);
            checkpoint(217);
            let cover_len = if bytes > 4 { (bytes as usize).min(cover.len()) } else { 0 };
            checkpoint(218);
            self.book_hero(x, cards_y, w, content_h, &cover[..cover_len],
                book.title(), book.author(), book.series(), book.percent);
        } else {
            text(x + 12, cards_y + cards_h / 2, w - 24, self.ls,
                b"No unfinished recent books", SMALL, CENTER, 0);
        }
    }
}