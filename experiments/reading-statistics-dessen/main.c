// Experimental Razor-Crest Reading Statistics for the Dessen RV32IM runtime.
// Read-only: use the firmware's versioned native records, never write them.
#include "dessen.h"
#include <stdint.h>
#include <stddef.h>

#define HIST 366
#define MAX_BOOKS 2048
#define FIRST_BOOK_ROW 77
#define BOOKS_PER_PAGE 5
#define WHITE 0
#define BLACK DSN_BLACK
#define MARGIN 16

// Freestanding RV32IM apps cannot link the target libc/libgcc, which may
// contain compressed RISC-V opcodes not supported by Dessen's VM.
// Supply the unsigned 64-bit shift/divide helpers without host dependencies.
uint64_t __lshrdi3(uint64_t v, int n) {
  uint32_t hi = (uint32_t)(v >> 32), lo = (uint32_t)v;
  if (n <= 0) return v;
  if (n >= 64) return 0;
  if (n >= 32) return (uint64_t)(hi >> (n - 32));
  return ((uint64_t)(hi >> n) << 32) | (lo >> n) | ((uint32_t)(hi << (32 - n)));
}
uint64_t __ashldi3(uint64_t v, int n) {
  uint32_t hi = (uint32_t)(v >> 32), lo = (uint32_t)v;
  if (n <= 0) return v;
  if (n >= 64) return 0;
  if (n >= 32) return (uint64_t)(lo << (n - 32)) << 32;
  return ((uint64_t)((hi << n) | (lo >> (32 - n))) << 32) | (uint32_t)(lo << n);
}
static uint64_t divide64(uint64_t n, uint64_t d, uint64_t *mod) {
  uint64_t q = 0, r = 0;
  if (!d) { if (mod) *mod = 0; return 0; }
  for (int bit = 63; bit >= 0; --bit) {
    r = (r << 1) | ((n >> bit) & 1u);
    if (r >= d) { r -= d; q |= (uint64_t)1 << bit; }
  }
  if (mod) *mod = r;
  return q;
}
uint64_t __udivdi3(uint64_t a, uint64_t b) { return divide64(a,b,0); }
uint64_t __umoddi3(uint64_t a, uint64_t b) { uint64_t r; divide64(a,b,&r); return r; }

static const char GLOBAL[] = "/.crosspoint/reading-stats/global_v2.bin";
static const char GLOBAL_BAK[] = "/.crosspoint/reading-stats/global_v2.bin.bak";
static const char BOOKS[] = "/.crosspoint/reading-stats/books_v1.bin";
static const char PERIODS[] = "/.crosspoint/reading-stats/time_global_v1.bin";

typedef struct {
  uint32_t magic, version, seq, sessions, seconds_lo, seconds_hi, turns, last_day;
  uint32_t daily[HIST];
  uint32_t checksum;
} Snapshot;
typedef struct {
  uint32_t magic, version, hash_lo, hash_hi;
  char path[176], title[72], author[48];
  uint32_t checksum;
} BookEntry;
typedef struct {
  uint32_t magic, version, seq, latest_day, totals[4], recent[7][4], checksum;
} TimeBuckets;

_Static_assert(sizeof(Snapshot) == 1500, "Snapshot ABI mismatch");
_Static_assert(sizeof(BookEntry) == 316, "Book index ABI mismatch");
_Static_assert(sizeof(TimeBuckets) == 148, "Time buckets ABI mismatch");

static Snapshot stats;
static Snapshot book_stats;
static TimeBuckets periods;
static BookEntry book;
static int loaded, have_periods, page, book_first, selected_book;
static int width, height;

static int slen(const char *s) { int n = 0; while (s[n]) ++n; return n; }
static char *cat(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *num(char *p, uint64_t value) {
  char r[24]; int n = 0;
  do { r[n++] = '0' + value % 10; value /= 10; } while (value && n < 23);
  while (n) *p++ = r[--n];
  return p;
}
static void label(int x, int y, const char *s, int font) {
  dsn_text_box(x, y, width - x - MARGIN, 35, s, slen(s), font, DSN_ALIGN_LEFT, 1, 0);
}
static void metric(int x, int y, const char *caption, uint64_t value, const char *unit) {
  char s[64], *p = s;
  p = num(p, value);
  if (unit && *unit) { p = cat(p, " "); p = cat(p, unit); }
  *p = 0;
  label(x, y, caption, DSN_FONT_SMALL);
  label(x, y + 23, s, DSN_FONT_TITLE);
}
static uint32_t fnv(const void *p, unsigned count) {
  const unsigned char *bytes = (const unsigned char*)p;
  uint32_t h = 2166136261u;
  for (unsigned i=0; i<count; ++i) h = (h ^ bytes[i]) * 16777619u;
  return h;
}
static int valid_snapshot(const Snapshot *s) {
  return s->magic == 0x32545352u && s->version == 2 &&
    s->checksum == fnv(s, offsetof(Snapshot, checksum));
}
static int valid_book(const BookEntry *b) {
  if (b->magic != 0x314b4252u || b->version != 1 ||
      b->checksum != fnv(b, offsetof(BookEntry, checksum))) return 0;
  int ok_path=0, ok_title=0, ok_author=0;
  for(int i=0;i<176;++i) if(!b->path[i]) {ok_path=1;break;}
  for(int i=0;i<72;++i) if(!b->title[i]) {ok_title=1;break;}
  for(int i=0;i<48;++i) if(!b->author[i]) {ok_author=1;break;}
  return ok_path && ok_title && ok_author;
}
static int load_book(int index) {
  int n = dsn_fs_read_at(BOOKS, sizeof(BOOKS)-1, (uint32_t)index * sizeof(BookEntry),
                         &book, sizeof book);
  return n == (int)sizeof book && valid_book(&book);
}
static int read_snapshot_path(const char *path, int path_len) {
  int n = dsn_fs_read(path, path_len, &stats, sizeof stats);
  return n == (int)sizeof stats && valid_snapshot(&stats);
}
static void load(void) {
  loaded = read_snapshot_path(GLOBAL, sizeof GLOBAL - 1) ||
           read_snapshot_path(GLOBAL_BAK, sizeof GLOBAL_BAK - 1);
  int n = dsn_fs_read(PERIODS, sizeof PERIODS - 1, &periods, sizeof periods);
  have_periods = n == sizeof periods && periods.magic == 0x31505452u && periods.version == 1 &&
      periods.checksum == fnv(&periods, offsetof(TimeBuckets, checksum));
}
static uint32_t day_seconds(uint32_t day) {
  if (!loaded || !day || day > stats.last_day || stats.last_day - day >= HIST) return 0;
  return stats.daily[day % HIST];
}
static uint32_t streak(void) {
  if (!loaded || !stats.last_day) return 0;
  uint32_t today = dsn_time() / 86400u + 1u;
  uint32_t day = today;
  if (day > stats.last_day && day - stats.last_day <= 1) day = stats.last_day;
  if (day > stats.last_day || !day_seconds(day)) return 0;
  unsigned n = 0;
  while (day && n < HIST && day_seconds(day)) { ++n; --day; }
  return n;
}
static void footer(const char *hint) {
  dsn_line(MARGIN, height - 50, width - MARGIN, height - 50, 1, BLACK);
  label(MARGIN, height - 39, hint, DSN_FONT_SMALL);
}
static void header(const char *title, int num_page) {
  char b[48], *p = b;
  dsn_clear();
  dsn_text_box(MARGIN, 9, width-145, 36, title, slen(title), DSN_FONT_TITLE, DSN_ALIGN_LEFT, 1, 0);
  p = cat(p, "Page "); p = num(p, num_page); p = cat(p, "/6"); *p = 0;
  dsn_text_box(width - 105, 10, 90, 22, b, slen(b), DSN_FONT_SMALL, DSN_ALIGN_RIGHT, 1, 0);
  dsn_line(MARGIN, 51, width - MARGIN, 51, 1, BLACK);
}
static void overview(void) {
  header("Reading Statistics", 1);
  if (!loaded) {
    label(MARGIN, 76, "No valid native reading statistics found.", DSN_FONT_BODY);
    label(MARGIN, 105, "Read a book to create statistics.", DSN_FONT_SMALL);
    footer("Next: activity   Back: exit");
    return;
  }
  uint64_t secs = ((uint64_t)stats.seconds_hi << 32) | stats.seconds_lo;
  int left = MARGIN, right = width/2 + 7;
  metric(left, 74, "Total reading", secs / 3600, "hours");
  metric(right, 74, "Reading sessions", stats.sessions, "");
  metric(left, 155, "Qualified turns", stats.turns, "");
  metric(right, 155, "Recent streak", streak(), "days");
  uint64_t week = 0;
  for (int i=0; i<7 && stats.last_day>(uint32_t)i; ++i)
    week += day_seconds(stats.last_day - i);
  metric(left, 236, "Latest 7 reading days", week / 60, "minutes");
  metric(right, 236, "Total minutes", secs / 60, "");
  footer("Prev/Next: pages   Back: exit");
}
static void activity(void) {
  header("Reading Activity", 2);
  if (!loaded) {
    label(MARGIN, 90, "No statistics yet.", DSN_FONT_BODY);
    footer("Prev/Next: pages   Back: exit");
    return;
  }
  label(MARGIN, 68, "Last 28 days (oldest to newest)", DSN_FONT_BODY);
  int chart_top=115, chart_bottom=height-108, chart_height=chart_bottom-chart_top;
  if(chart_height<70) chart_height=70;
  uint32_t peak=1;
  uint32_t end=stats.last_day;
  for(int i=0;i<28;++i) { uint32_t v= end>=(uint32_t)(27-i)?day_seconds(end-(27-i)):0; if(v>peak)peak=v; }
  int gap=3, cell=(width-2*MARGIN)/28;
  if(cell<2) cell=2;
  for(int i=0;i<28;++i) {
    uint32_t v=end>=(uint32_t)(27-i)?day_seconds(end-(27-i)):0;
    int x=MARGIN+i*cell, bar=(int)((uint64_t)v*(chart_height-8)/peak);
    if(bar) dsn_fill(x,chart_bottom-bar,cell>gap?cell-gap:1,bar,BLACK);
  }
  dsn_line(MARGIN, chart_bottom, width-MARGIN,chart_bottom, 1,BLACK);
  char b[60], *p=b;
  p=cat(p,"Peak: "); p=num(p,peak/60); p=cat(p," min/day");*p=0;
  label(MARGIN,height-91,b,DSN_FONT_BODY);
  footer("Prev/Next: pages   Back: exit");
}
// The firmware records local civil-day ordinals; the app clock supplies the
// current UTC month. Near local midnight the calendar may lag one day.
static int month_days(int y,int m) {
  static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  return days[m-1]+(m==2&&((y%4==0&&y%100!=0)||y%400==0));
}
static uint32_t civil_ordinal(int y,int m,int d) {
  y-=m<=2;
  int era=y/400,yoe=y-era*400,mp=m>2?m-3:m+9;
  int doy=(153*mp+2)/5+d-1;
  return (uint32_t)(era*146097+yoe*365+yoe/4-yoe/100+doy-719468+1);
}
static void calendar_page(void) {
  header("Reading Calendar",3);
  int z=(int)(dsn_time()/86400u)+719468,era=z/146097;
  int doe=z-era*146097;
  int yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
  int year=yoe+era*400;
  int doy=doe-(365*yoe+yoe/4-yoe/100);
  int mp=(5*doy+2)/153;
  int month=mp+(mp<10?3:-9);
  year+=month<=2;
  if(year<2020||year>2100) {
    label(MARGIN,83,"Set device time to view calendar.",DSN_FONT_BODY);
    footer("Prev/Next: pages   Back: exit"); return;
  }
  static const char *months[]={"January","February","March","April","May","June",
    "July","August","September","October","November","December"};
  static const char *weekdays[]={"M","T","W","T","F","S","S"};
  char title[50],*p=title;
  p=cat(p,months[month-1]);p=cat(p," ");p=num(p,year);*p=0;
  label(MARGIN,65,title,DSN_FONT_BODY);
  int cell_w=(width-2*MARGIN)/7,cell_h=(height-191)/6;
  if(cell_h<20)cell_h=20;
  int first=(int)civil_ordinal(year,month,1);
  int offset=(first-1+3)%7,days=month_days(year,month);
  uint64_t total=0;int active=0;
  for(int c=0;c<7;++c)
    dsn_text_box(MARGIN+c*cell_w,90,cell_w,18,weekdays[c],1,DSN_FONT_SMALL,DSN_ALIGN_CENTER,1,0);
  for(int day=1;day<=days;++day) {
    int slot=offset+day-1,col=slot%7,row=slot/7;
    int x=MARGIN+col*cell_w,y=112+row*cell_h;
    if(y+cell_h>=height-55)break;
    char n[4],*q=n;q=num(q,day);*q=0;
    dsn_stroke(x+1,y,cell_w-3,cell_h-3,BLACK,1,0,0);
    dsn_text_box(x+2,y+3,cell_w-7,20,n,slen(n),DSN_FONT_SMALL,DSN_ALIGN_CENTER,1,0);
    uint32_t secs=day_seconds((uint32_t)(first+day-1));
    if(secs) { ++active;total+=secs; dsn_fill(x+4,y+cell_h-10,cell_w-9,5,BLACK); }
  }
  char summary[70];p=summary;p=cat(p,"Active days: ");p=num(p,active);
  p=cat(p,"   Read: ");p=num(p,total/60);p=cat(p," min");*p=0;
  label(MARGIN,height-82,summary,DSN_FONT_SMALL);
  footer("Prev/Next: pages   Back: exit");
}
static void periods_page(void) {
  header("Time of Day", 4);
  if(!have_periods) {
    label(MARGIN,85,"No timed reading sessions yet.",DSN_FONT_BODY);
    footer("Prev/Next: pages   Back: exit");
    return;
  }
  const char *names[4]={"Morning (06-12)","Afternoon (12-17)","Evening (17-21)","Night (21-06)"};
  uint32_t peak=1;
  for(int i=0;i<4;++i)if(periods.totals[i]>peak)peak=periods.totals[i];
  int room=height-150;
  int row=room/4;
  if(row<42)row=42;
  int bar_width=width-2*MARGIN;
  for(int i=0;i<4;++i) {
    int y=74+i*row;
    char txt[75],*p=txt;
    p=cat(p,names[i]);p=cat(p,": ");p=num(p,periods.totals[i]/60);p=cat(p," min");*p=0;
    label(MARGIN,y,txt,DSN_FONT_BODY);
    int bar=(int)((uint64_t)bar_width*periods.totals[i]/peak);
    if(bar)dsn_fill(MARGIN,y+27,bar,10,BLACK);
  }
  footer("Prev/Next: pages   Back: exit");
}
static void books(void) {
  header("Books", 5);
  int row_h=(height-155)/BOOKS_PER_PAGE;
  if(row_h<37) row_h=37;
  int any=0;
  for(int i=0;i<BOOKS_PER_PAGE;++i) {
    int idx=book_first+i;
    if(idx>=MAX_BOOKS || !load_book(idx)) break;
    any=1;
    int y=FIRST_BOOK_ROW+i*row_h;
    char title[72]; int j=0;
    while(j<71 && book.title[j]) {title[j]=book.title[j];++j;}title[j]=0;
    label(MARGIN,y,title,DSN_FONT_BODY);
    label(MARGIN,y+22,book.author[0]?book.author:"Unknown author",DSN_FONT_SMALL);
    if(i<BOOKS_PER_PAGE-1) dsn_line(MARGIN,y+row_h-5,width-MARGIN,y+row_h-5,1,BLACK);
  }
  if(!any) label(MARGIN,90,"No indexed books yet.",DSN_FONT_BODY);
  char b[64],*p=b;
  p=cat(p,"Books ");p=num(p,book_first+1);p=cat(p," onwards");*p=0;
  label(MARGIN,height-93,b,DSN_FONT_SMALL);
  footer("Confirm: next  Next: details");
}
static void hex8(char **p, uint32_t n) {
  static const char hex[]="0123456789abcdef";
  for(int shift=28;shift>=0;shift-=4) *(*p)++=hex[(n>>shift)&15];
}
static void book_details(void) {
  header("Book Statistics",6);
  if(!load_book(selected_book)) {
    label(MARGIN,88,"Book not found.",DSN_FONT_BODY);
    footer("Back: books   Next: overview");return;
  }
  label(MARGIN,73,book.title,DSN_FONT_BODY);
  label(MARGIN,105,book.author[0]?book.author:"Unknown author",DSN_FONT_SMALL);
  char filename[100],*p=filename;
  p=cat(p,"/.crosspoint/reading-stats/book_");
  hex8(&p,book.hash_hi);hex8(&p,book.hash_lo);
  p=cat(p,"_v2.bin");*p=0;
  int size=dsn_fs_read(filename,slen(filename),&book_stats,sizeof book_stats);
  int valid=size==(int)sizeof book_stats&&valid_snapshot(&book_stats);
  if(!valid) {
    p=cat(p,".bak");*p=0;
    size=dsn_fs_read(filename,slen(filename),&book_stats,sizeof book_stats);
    valid=size==(int)sizeof book_stats&&valid_snapshot(&book_stats);
  }
  if(valid) {
    uint64_t secs=((uint64_t)book_stats.seconds_hi<<32)|book_stats.seconds_lo;
    metric(MARGIN,145,"Reading time",secs/60,"minutes");
    metric(width/2+7,145,"Sessions",book_stats.sessions,"");
    metric(MARGIN,240,"Page turns",book_stats.turns,"");
    uint64_t recent=0;
    for(int i=0;i<7&&book_stats.last_day>(uint32_t)i;++i)
      recent+=book_stats.daily[(book_stats.last_day-i)%HIST];
    metric(width/2+7,240,"Last 7 days",recent/60,"minutes");
  } else label(MARGIN,160,"No recorded sessions for this book.",DSN_FONT_BODY);
  footer("Confirm: next book  Back: books");
}
static void draw(void) {
  if(page==0)overview(); else if(page==1)activity(); else if(page==2)calendar_page();
  else if(page==3)periods_page(); else if(page==4)books(); else book_details();
  dsn_refresh();
}
int main(void) {
  width=dsn_screen_width(); height=dsn_screen_height();
  if(width<200||height<200)return 1;
  load(); draw();
  for(;;) {
    int ev=dsn_wait_event(),kind=DSN_EV_TYPE(ev);
    if(kind==DSN_EV_BACK) {
      if(page==5)page=4;
      else return 0;
    } else if(kind==DSN_EV_CONFIRM && page==4) {
      if(load_book(book_first+BOOKS_PER_PAGE))book_first+=BOOKS_PER_PAGE;
      else book_first=0;
      selected_book=book_first;
    } else if(kind==DSN_EV_CONFIRM && page==5) {
      if(!load_book(selected_book+1))selected_book=0;
      else ++selected_book;
    } else if(kind==DSN_EV_NEXT || kind==DSN_EV_CONFIRM) {
      if(page==4)selected_book=book_first;
      page=(page+1)%6;
    } else if(kind==DSN_EV_PREV) page=(page+5)%6;
    else if(kind==DSN_EV_TAP) {
      int x=DSN_EV_X(ev),y=DSN_EV_Y(ev);
      if(y<55 && x<MARGIN+70) {
        if(page==5)page=4; else return 0;
      } else if(page==4 && y>=FIRST_BOOK_ROW && y<height-90) {
        int row_h=(height-155)/BOOKS_PER_PAGE;
        if(row_h<37)row_h=37;
        int index=book_first+(y-FIRST_BOOK_ROW)/row_h;
        if(index<book_first+BOOKS_PER_PAGE&&load_book(index)) {
          selected_book=index;page=5;
        }
      } else if(page==5 && y>=55) {
        if(!load_book(selected_book+1))selected_book=0;
        else ++selected_book;
      } else page=(page+1)%6;
    } else continue;
    draw();
  }
}
