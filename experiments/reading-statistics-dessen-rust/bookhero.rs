// Shared Current Book / Continue Reading hero geometry. Neither caller owns
// its own typography or progress-bar spacing.
impl Ui {
    fn footer_height(&self) -> i32 {
        if !self.touch && self.buttons { 36 } else { 0 }
    }

    fn content_bottom(&self) -> i32 { self.h - self.footer_height() }

    // The X3/X4 four-button legend belongs outside the content cards.
    // Physical side buttons always page; front Left/Right control books only
    // on Overall Statistics when more than one book can be selected.
    fn draw_footer(&self, page: i32, books: usize) {
        if self.footer_height() == 0 { return; }
        let y = self.h - 34;
        let book_controls = page == 0 && books > 1;
        let can_open = (page == 0 && books > 0) || page == 2 || page == 6;
        let labels: [&[u8]; 4] = [
            b"Back", if can_open { b"Open" } else { b"" },
            if book_controls { b"Prev Book" } else { b"Prev Page" },
            if book_controls { b"Next Book" } else { b"Next Page" },
        ];
        // Start from measured label widths, then distribute the spare space.
        // Equal quarter-width cells truncated "Prev Book"/"Next Book" on X3.
        let mut widths = [0i32; 4];
        let mut needed = 0i32;
        for i in 0..4 {
            widths[i] = ((dsn_measure_text(labels[i], SMALL, BOLD) >> 16) as i32 + 15)
                .max(24);
            needed += widths[i];
        }
        let spare = (self.w - needed).max(0);
        let mut x = 0;
        for i in 0..4 {
            let width = if i == 3 { self.w - x }
                else { widths[i] + spare / 4 };
            dsn_stroke(x + 1, y, (width - 2).max(1), 32, DARK, 1, 0, 0);
            text(x + 5, y + (32 - self.ls) / 2, (width - 10).max(1),
                self.ls, labels[i], SMALL, CENTER, BOLD);
            x += width;
        }
    }

    // Minimum height for two title lines, one author line, two series
    // lines, and a distinct progress row at the smallest native UI font.
    fn book_hero_min_height(&self) -> i32 {
        7 + 5 * self.ls + 8 + self.lt / 2 + 20
    }

    fn book_hero(&self, x: i32, y: i32, w: i32, h: i32,
                 cover: &[u8], title: &[u8], author: &[u8],
                 series: &[u8], percent: i32) {
        // Reserve the left column for a full portrait cover. The progress
        // strip belongs to the metadata column, never underneath the cover.
        let cover_x = x + 10;
        let cover_y = y + 10;
        let cover_col = (w / 3).max(45);
        let cover_max_h = (h - 19).max(1);
        let mut cover_w = 0i32;
        let mut cover_h = 0i32;
        if cover.len() > 4 {
            cover_w = cover[0] as i32 | (cover[1] as i32) << 8;
            cover_h = cover[2] as i32 | (cover[3] as i32) << 8;
        }
        let show_cover = cover_w > 0 && cover_h > 0 &&
            cover_w <= cover_col - 10 && cover_h <= cover_max_h;
        if show_cover {
            dsn_bitmap(cover_x + (cover_col - cover_w) / 2, cover_y,
                cover_w, cover_h, &cover[4..], cover_w, cover_h, 1, 0, BLACK, 0);
        } else {
            let ph = cover_max_h.min((cover_col - 8) * 4 / 3);
            dsn_stroke(cover_x + 5, cover_y, cover_col - 10, ph, DARK, 1, 0, 0);
            text(cover_x + 7, cover_y + ph / 2 - self.ls / 2,
                cover_col - 14, self.ls, b"No cover", SMALL, CENTER, 0);
        }
        let tx = cover_x + cover_col + 9;
        let tw = (x + w - 13 - tx).max(1);
        // Anchor progress to the BOTTOM of the hero, not above the metadata.
        // Reserve two title lines and two series lines (plus author) first.
        // If the card is compact, use a smaller metadata font rather than
        // silently dropping the second title line.
        let pct_h = dsn_line_height(TITLE);
        let progress_y = y + h - pct_h / 2 - 10;
        let title_y = y + 7;
        let text_bottom = if percent >= 0 { progress_y - 5 } else { y + h - 8 };
        let series_lines = if series.is_empty() { 0 } else { 2 };
        let needed_body = self.lb * (3 + series_lines) + 8;
        let use_small = text_bottom - title_y < needed_body;
        let metadata_font = if use_small { SMALL } else { BODY };
        let line_h = if use_small { self.ls } else { self.lb };
        let title_h = line_h * 2;
        let author_y = title_y + title_h + 3;
        let series_y = author_y + line_h + 5;
        // Both text boxes always accept two lines. A short title still
        // reserves its second line so the author and series never jump.
        if title_y + title_h <= text_bottom {
            dsn_text_box(tx, title_y, tw, title_h, title, metadata_font, LEFT, 2, BOLD);
        }
        if author_y + line_h <= text_bottom {
            text(tx, author_y, tw, line_h, author, metadata_font, LEFT, 0);
        }
        if series_lines > 0 && series_y + line_h * 2 <= text_bottom {
            dsn_text_box(tx, series_y, tw, line_h * 2, series, metadata_font, LEFT, 2, 0);
        }
        if percent >= 0 {
            let pct_w = ((dsn_measure_text(b"100%", TITLE, BOLD) >> 16) as i32 + 8)
                .clamp(48, 78);
            let pct_x = x + w - pct_w - 11;
            let divider_x = pct_x - 8;
            let bar_w = (divider_x - tx - 9).max(1);
            dsn_fill(tx, progress_y, bar_w, 7, LIGHT);
            if percent > 0 {
                dsn_fill(tx, progress_y, bar_w * percent.min(100) / 100, 7, BLACK);
            }
            dsn_line(divider_x, progress_y - 9, divider_x, (progress_y + 15).min(y + h - 8), 1, DARK);
            let pct_h = dsn_line_height(TITLE);
            text(pct_x, progress_y + 3 - pct_h / 2, pct_w, pct_h,
                S::new().n(percent as u32).s(b"%").get(), TITLE, RIGHT, BOLD);
        }
    }
}
