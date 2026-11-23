// QR bitmaps are encoded by the native firmware through the bounded Dessen
// syscall. The guest only formats public, short-lived reader PIN links.
fn draw_pairing_qr(code: &[u8; 9], web_login: bool, x: i32, y: i32, side: i32) -> bool {
    const PAIR: &[u8] = b"https://sync.techy-notes.com/link?code=";
    const LOGIN: &[u8] = b"https://sync.techy-notes.com/signin?code=";
    const FRAGMENT: &[u8] = b"#reader-login";
    let prefix = if web_login { LOGIN } else { PAIR };
    let mut url = [0u8; 72];
    let mut n = prefix.len();
    url[..n].copy_from_slice(prefix);
    url[n..n + 9].copy_from_slice(code);
    n += 9;
    if web_login {
        url[n..n + FRAGMENT.len()].copy_from_slice(FRAGMENT);
        n += FRAGMENT.len();
    }
    let mut pixels = [0u8; 176];
    let bytes = dsn_qr_bitmap(&url[..n], &mut pixels);
    if bytes < 4 || bytes as usize > pixels.len() { return false; }
    let bw = u16::from_le_bytes([pixels[0], pixels[1]]) as i32;
    let bh = u16::from_le_bytes([pixels[2], pixels[3]]) as i32;
    if bw != 33 || bh != 33 || bytes as usize != 4 + ((bw as usize + 7) / 8) * bh as usize {
        return false;
    }
    // Four-module quiet zone with integer scaling for e-ink readability.
    let scale = ((side - 32) / 33).max(2);
    let length = scale * 33;
    let pad = 4 * scale;
    dsn_fill(x, y, length + 2 * pad, length + 2 * pad, 0);
    dsn_bitmap(x + pad, y + pad, length, length, &pixels[4..bytes as usize],
               bw, bh, 1, 1, BLACK, 0) == 0
}
