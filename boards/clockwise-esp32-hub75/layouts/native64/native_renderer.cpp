#include "native_renderer.h"
#include "display/scene.h"
#include "ported/features/pixel_screen/activity.h"
#include "ported/features/pixel_screen/layout_data.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace pixel_screen { namespace {
constexpr uint32_t black=0,white=0xffffff,cyan=0x00d6ee,blue=0x1468f3,yellow=0xffc400,green=0x00ed48;
void dot(uint32_t *f,int x,int y,uint32_t c){if(x>=0&&x<64&&y>=0&&y<64)f[y*64+x]=c;}
void box(uint32_t *f,int x,int y,int w,int h,uint32_t c){for(int j=0;j<h;j++)for(int i=0;i<w;i++)dot(f,x+i,y+j,c);}
constexpr uint32_t pale = 0xb4cfd9, layerInk = 0x38dbff, nozzleInk = 0xff8c5c, bedInk = 0xf9c260;

const print_layout::Glyph &glyphFor(char ch) {
    for (const auto &glyph : print_layout::font)
        if (glyph.ch == ch) return glyph;
    for (const auto &glyph : print_layout::font)
        if (glyph.ch == ' ') return glyph;
    return print_layout::font[0];
}

int length(const char *value, int scale = 1) {
    int width = 0;
    for (; *value; ++value) width += glyphFor(*value).width + 1;
    return std::max(0, width - 1) * scale;
}

void text(uint32_t *frame, int x, int y, const char *value, uint32_t color,
          int scale = 1, uint32_t lower = 0) {
    for (; *value; ++value) {
        const auto &glyph = glyphFor(*value);
        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < glyph.width; ++col)
                if (glyph.rows[row] & (1u << (glyph.width - col - 1)))
                    box(frame, x + col * scale, y + row * scale, scale, scale, row >= 3 && lower ? lower : color);
        x += (glyph.width + 1) * scale;
    }
}

void center(uint32_t *frame, int y, const char *value, uint32_t color, int scale) {
    text(frame, (64 - length(value, scale)) / 2, y, value, color, scale);
}

void field(uint32_t *frame, int x, char label, const char *value, uint32_t color) {
    const char name[] = {label, 0};
    text(frame, x, 2, name, color);
    text(frame, x + 5, 2, value, white, 1, pale);
}

void number(char *output, size_t size, int value) {
    if (value < 0) std::snprintf(output, size, "--");
    else std::snprintf(output, size, "%d", value);
}

void layerText(const Snapshot &s, char *output, size_t size) {
    if (s.currentLayer < 0 || s.totalLayers < 1) std::snprintf(output, size, "--");
    else std::snprintf(output, size, "%d/%d", s.currentLayer, s.totalLayers);
}

void timeText(const Snapshot &s, char *output, size_t size) {
    if (s.leftMinutes < 0) std::snprintf(output, size, "--:--");
    else if (s.leftMinutes >= 1440) std::snprintf(output, size, "%dD%02dH", s.leftMinutes / 1440, s.leftMinutes % 1440 / 60);
    else std::snprintf(output, size, "%d:%02d", s.leftMinutes / 60, s.leftMinutes % 60);
}

const char *activityLabel(Activity activity, const Snapshot &s) {
    switch (activity) {
        case Activity::Heating: return s.stage == 49 ? "CH HEAT" : "HEAT";
        case Activity::Homing: return "HOME";
        case Activity::Leveling: return "LEVEL";
        case Activity::Cleaning: return "CLEAN";
        case Activity::Filament: return "FEED";
        case Activity::Calibration: return "CALIB";
        case Activity::Checking: return "CHECK";
        default: return s.state == State::Preparing ? "WAIT" : s.headParked ? "FINISH" : s.model;
    }
}

void header(uint32_t *frame, const Snapshot &s, unsigned phase) {
    box(frame, 0, 0, 64, 14, black);
    const bool active = s.state == State::Preparing || s.state == State::Printing;
    const bool warm = s.state == State::Preparing || (s.state == State::Printing && s.currentLayer < 1);
    box(frame, 58, 2, 4, 5, s.state == State::Error ? 0xff364f :
        warm || s.state == State::Paused ? yellow :
        s.state == State::Offline || s.state == State::Cancelled ? 0x5b6771 : green);
    if (!active) {
        text(frame, 2, 2, s.model, white);
        return;
    }

    char layers[28], nozzle[16], bed[16], current[16];
    layerText(s, layers, sizeof(layers));
    number(nozzle, sizeof(nozzle), s.nozzle);
    number(bed, sizeof(bed), s.bed);
    number(current, sizeof(current), s.currentLayer);
    switch (phase / 16 % 3) {
        case 1: field(frame, 2, 'L', layers, layerInk); break;
        case 2:
            field(frame, 2, 'N', nozzle, nozzleInk);
            field(frame, 30, 'B', bed, bedInk);
            break;
        default:
            text(frame, 2, 2, activityLabel(activityFor(s), s), warm ? yellow : white);
            field(frame, 38, s.progressLayout ? 'N' : 'L',
                  s.progressLayout ? nozzle : current, s.progressLayout ? nozzleInk : layerInk);
            break;
    }
}

void printFooter(uint32_t *frame, const Snapshot &s, bool warm) {
    char percent[8], remaining[24];
    std::snprintf(percent, sizeof(percent), "%u%%", unsigned(s.percent));
    timeText(s, remaining, sizeof(remaining));
    box(frame, 0, 53, 64, 11, black);
    text(frame, 2, 54, percent, warm ? yellow : green, 2);
    const int scale = length(percent, 2) + length(remaining, 2) <= 56 ? 2 : 1;
    text(frame, 62 - length(remaining, scale), scale == 2 ? 54 : 57, remaining, white, scale, pale);
}

void progressView(uint32_t *frame, const Snapshot &s, unsigned phase) {
    constexpr uint32_t rainbow[] = {0x00ceff, 0x1472ff, 0x8740ff, 0xf632dc, 0xff6b24, 0xffcd00, 0x00ef65};
    box(frame, 0, 9, 64, 55, black);
    char percent[8], remaining[24], layers[28], label[32];
    std::snprintf(percent, sizeof(percent), "%u%%", unsigned(s.percent));
    center(frame, 14, percent, green, 4);
    box(frame, 2, 35, 60, 3, s.filamentKnown ? visibleFilamentColor(s.filamentColor) : yellow);
    const unsigned fill = (60u * s.percent + 50) / 100;
    const unsigned shift = (phase / 3) % 28;
    for (unsigned x = 0; x < fill; ++x)
        box(frame, 2 + x, 35, 1, 3, rainbow[(x + 28 - shift) % 28 / 4]);

    layerText(s, layers, sizeof(layers));
    std::snprintf(label, sizeof(label), "L%s", layers);
    center(frame, 43, label, layerInk, length(label, 2) <= 60 ? 2 : 1);
    timeText(s, remaining, sizeof(remaining));
    center(frame, 54, remaining, pale, length(remaining, 2) <= 60 ? 2 : 1);
}

void chamberHeat(uint32_t *f,const Snapshot &s,unsigned phase){
 if(!s.chamberHeating||s.headParked)return;
 for(int side:{8,52})for(int k=0;k<16;k++){
  if((k/2+phase/2)%4==0)continue;
  const int bend=(k/2+phase/3)%4>=2?2:0;
  box(f,side+bend,43-k,2,1,(k/2+phase)%3?rgb(12):rgb(6));
 }
}
// One frame for every scene which contains a printer.
void chamber(uint32_t *f,const Snapshot &s,unsigned phase){
 box(f,0,10,64,43,black);
 box(f,2,10,60,4,rgb(16));box(f,2,14,4,34,rgb(9));
 box(f,58,14,4,34,rgb(17));box(f,2,48,60,4,rgb(18));
 box(f,9,16,46,2,rgb(20));box(f,11,46,42,2,rgb(21));
 const int filled=(int(std::min(100u,unsigned(s.percent)))*34+50)/100;
 for(int y=14;y<48;y++)box(f,4,y,2,1,y>=48-filled?(s.filamentKnown?visibleFilamentColor(s.filamentColor):rgb(6)):rgb(25));
 box(f,53,12,2,2,phase%8<4?rgb(26):rgb(9));
}
} // namespace

void render64(const Snapshot &input,unsigned phase,uint32_t *f){
 Snapshot value=input;value.percent=std::min(100u,unsigned(value.percent));const Snapshot &s=value;
 std::fill(f,f+pixels64,black);
 if(s.state==State::Idle){
  // Scale the shared idle artwork exactly, including the READY glyphs.
  uint8_t idle[pixels];renderScene(s,phase,idle);
  for(unsigned y=7;y<32;y++)for(unsigned x=0;x<32;x++)
   box(f,x*2,y*2,2,2,rgb(idle[y*32+x],s.filamentColor,s.feedColor));
  header(f,s,phase);
  return;
 }
 // Existing state artwork remains unchanged outside the printing chamber.
 uint8_t base[pixels];renderScene(s,phase,base);
 for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++)
  box(f,x*2,y*2,2,2,rgb(base[y*32+x],s.filamentColor,s.feedColor));
 header(f,s,phase);
 if(s.state==State::Error){
  box(f,4,16,4,4,black);box(f,56,16,4,4,black);
  if(phase%10<5){box(f,3,17,4,4,0xffa000);box(f,57,17,4,4,0xffa000);}
 }
 const Activity activity=activityFor(s);
 const bool chamberStage=activity==Activity::Heating&&s.stage==49;
 if(s.state==State::Printing&&activity==Activity::None&&s.progressLayout){
  progressView(f,s,phase);
  return;
 }
 const bool printing=s.state==State::Printing&&((activity==Activity::None&&!s.progressLayout)||chamberStage);
 const bool preparing=s.state==State::Preparing||(activity!=Activity::None&&!chamberStage);
 if(!printing&&!preparing)return;
 chamber(f,s,phase);
 if(preparing){
  // Fit the original action inside the common enlarged chamber. Mapping all
  // pixels together preserves contact between nozzle, arrows, bed and brush.
  for(int y=18;y<46;y++)for(int x=9;x<55;x++){
   const int sx=6+(x-9)*20/46,sy=11+(y-18)*12/28;
   dot(f,x,y,rgb(base[sy*32+sx],s.filamentColor,s.feedColor));
  }
  for(int x=11;x<53;x++)box(f,x,46,1,2,rgb(base[23*32+7+(x-11)*18/42],s.filamentColor,s.feedColor));
  if(activity==Activity::Cleaning){
   // Integer 2x strokes keep all seven brush teeth identical to the 32px artwork.
   box(f,9,18,46,28,black);box(f,18,44,30,2,rgb(6));
   for(int k=0;k<7;k++)box(f,20+k*4,40,2,4,rgb(12));
   const unsigned t=phase%24;const int x=16+int(t<=12?t:24-t)*2;
   box(f,x,32,10,4,white);
   if(s.filamentKnown)box(f,x+4,32+int(phase/2%2)*2,2,2,visibleFilamentColor(s.feedColor));
   box(f,x+2,36,6,2,rgb(12));box(f,x+4,38,2,2,s.filamentKnown?visibleFilamentColor(s.filamentColor):rgb(6));
  }
  chamberHeat(f,s,phase);
  printFooter(f,s,s.state==State::Preparing || s.currentLayer<1);
  return;
 }
 unsigned h=0;
 if(s.totalLayers>0&&s.currentLayer>0)h=std::min(24u,unsigned((uint64_t(s.currentLayer)*24+s.totalLayers-1)/s.totalLayers));
 else if(s.currentLayer>0)h=std::min(24u,unsigned(s.percent)*24/100);
 const uint32_t stripes[]={0x00e9f2,0x00f162,0xffdc00,0xff8920,0xf226cd,0xa52dff,0x167cff};
 for(unsigned row=0;row<h;row++)box(f,20,45-row,24,1,stripes[(row/3)%7]);
 int tip=45-int(h),x=s.headParked?31:22+int(phase%24<=12?phase%24:24-phase%24)*20/12;
 const int top=tip-7;box(f,x-4,top,10,4,white);
 if(s.filamentKnown&&!s.headParked)box(f,x,top+int(phase/2%2)*2,2,2,visibleFilamentColor(s.feedColor));
 box(f,x-2,tip-3,6,2,0xffa000);
 box(f,x,tip-1,2,2,s.filamentKnown?visibleFilamentColor(s.filamentColor):yellow);
 chamberHeat(f,s,phase);
 printFooter(f,s,s.currentLayer<1);
}

} // namespace pixel_screen
