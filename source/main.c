#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <citro2d.h>
#include <stdarg.h>
#include <time.h>

#define TOP_SCREEN_WIDTH 400
#define TOP_SCREEN_HEIGHT 240
#define BOTTOM_SCREEN_WIDTH 320
#define BOTTOM_SCREEN_HEIGHT 240

#define BUFFER_SIZE 64

#define LOG_LINES 12
#define LOG_COLUMNS 64

#define TARGET_FRAMERATE 60.0f

u64 updateTime = 1000;

C2D_TextBuf staticBuffer, dynamicBuffer, logBuffer;
C2D_Text batteryErrorText, sdText, nandText, exitText, batteryText;

u32 errCol, clearCol, greenCol, chargingCol, blackCol, barCol, backCol;
float sdFilled = 0, nandFilled = 0;
bool updateBottom = true;

char logg[LOG_LINES][LOG_COLUMNS];
C2D_Text logText[LOG_LINES];

typedef struct {
    u32 key;
    int initialDelay; //In frames
    int repeatDelay; //In frames
    int timer;
    bool repeating;
} KeyRepeat;

const char* const months[12] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
const char* const weekDays[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const u16 daysAtStartOfMonthLUT[12] = {
	0   % 7, //january    31
	31  % 7, //february   28+1(leap year)
	59  % 7, //march      31
	90  % 7, //april      30
	120 % 7, //may        31
	151 % 7, //june       30
	181 % 7, //july       31
	212 % 7, //august     31
	243 % 7, //september  30
	273 % 7, //october    31
	304 % 7, //november   30
	334 % 7  //december   31
};

static inline bool isLeapYear(int year){
	return (year%4) == 0 && !((year%100) == 0 && (year%400) != 0);
}

static inline int getDayOfWeek(int day, int month, int year){
	//http://en.wikipedia.org/wiki/Calculating_the_day_of_the_week
	day += 2*(3-((year/100)%4));
	year %= 100;
	day += year + (year/4);
	day += daysAtStartOfMonthLUT[month] - (isLeapYear(year) && (month <= 1));
	return day % 7;
}

bool krUpdate(KeyRepeat* kr, u32 held, u32 down){
    if (down & kr->key) {
        // First press
        kr->timer = kr->initialDelay;
        kr->repeating = false;
        return true;
    }

    if (!(held & kr->key)) {
        // Released
        kr->timer = 0;
        kr->repeating = false;
        return false;
    }

    // Key is being held
    if(kr->timer > 0) {
        kr->timer--;
        return false;
    }

    kr->timer = kr->repeatDelay;
    kr->repeating = true;

    return true;
}

void krCreate(KeyRepeat *kr, u32 key, float initialDelaySeconds, float repeatDelaySeconds){
	kr->key = key;
	kr->initialDelay = (int) (initialDelaySeconds * TARGET_FRAMERATE);
	kr->repeatDelay = (int) (repeatDelaySeconds * TARGET_FRAMERATE);
	
	kr->timer = 0;
	kr->repeating = 0;
}

void generateTextDynamic(C2D_Text* target, const char* format, ...){
	char buf[BUFFER_SIZE];
	
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	
	C2D_TextParse(target, dynamicBuffer, buf);
	C2D_TextOptimize(target);
}

void generateTextStatic(C2D_Text* target, const char* format, ...){
	char buf[BUFFER_SIZE];
	
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	
	C2D_TextParse(target, staticBuffer, buf);
	C2D_TextOptimize(target);
}

void drawBar(float yPos, u32 color, float filled, C2D_Text* text){
	const float width = 300.0f;
	const float height = 30.0f;
	const float left = 10.0f;
	const float border = 3.0f;
	
	C2D_DrawRectSolid(left, yPos, 0, width, height, blackCol);
	C2D_DrawRectSolid(left + border, yPos + border, 0, width - border * 2, height - border * 2, backCol);
	C2D_DrawRectSolid(left + border, yPos + border, 0, (width - border * 2) * filled, height - border * 2, color);
	
	C2D_DrawText(text, C2D_AtBaseline | C2D_AlignCenter, left + width / 2, yPos + height / 2 + 6.0f, -0.0f, 0.45f, 0.45f);
	
	C2D_Text percentage;
	generateTextDynamic(&percentage, "%.2f%%", filled * 100.0f);
	C2D_DrawText(&percentage, C2D_AtBaseline, left + width + 6.0, yPos + height / 2 + 6.0f, -0.0f, 0.5f, 0.5f);
}

void addLog(const char* format, ...){
	char buf[LOG_COLUMNS];
	
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	
	//Move up
	for(int i = 0; i < LOG_LINES - 1; i++){
		strcpy(logg[i], logg[i + 1]);
	}
	strcpy(logg[LOG_LINES - 1], buf);
	
	//Generate citro text
	C2D_TextBufClear(logBuffer);
	for(int i = 0; i < LOG_LINES; i++){
		if(logg[i][0] == '\0')
			continue;
		C2D_TextParse(&logText[i], logBuffer, logg[i]);
		C2D_TextOptimize(&logText[i]);
	}
	
	updateBottom = true;
}

void clearLog(){
	for(int i = 0; i < LOG_LINES; i++){
		logg[i][0] = '\0';
	}
	
	updateBottom = true;
}

void sceneInit(){
	staticBuffer = C2D_TextBufNew(4096);
	dynamicBuffer = C2D_TextBufNew(4096);
	logBuffer = C2D_TextBufNew(LOG_LINES * LOG_COLUMNS);
	
	generateTextStatic(&batteryErrorText, "Battery error");
	generateTextStatic(&exitText, "Press START to exit");
	generateTextStatic(&batteryText, "Battery");
	
	FS_ArchiveResource sd;
	Result res = FSUSER_GetSdmcArchiveResource(&sd);
	if(R_SUCCEEDED(res)){
		u64 totalBytes = (u64)sd.clusterSize * sd.totalClusters;
		u64 freeBytes = (u64)sd.clusterSize * sd.freeClusters;
		u64 usedBytes = totalBytes - freeBytes;
		sdFilled = (float) usedBytes / totalBytes;
		
		generateTextStatic(&sdText, "SDMC: %.2f/%.2f MB", usedBytes / 1048576.0d, totalBytes / 1048576.0d);
		addLog("SDMC: %llu B free out of %llu B", freeBytes, totalBytes);
	}else{
		generateTextStatic(&sdText, "SDMC error");
	}
	
	FS_ArchiveResource nand;
	res = FSUSER_GetNandArchiveResource(&nand);
	if(R_SUCCEEDED(res)){
		u64 totalBytes = (u64)nand.clusterSize * nand.totalClusters;
		u64 freeBytes = (u64)nand.clusterSize * nand.freeClusters;
		u64 usedBytes = totalBytes - freeBytes;
		nandFilled = (float) usedBytes / totalBytes;
		
		generateTextStatic(&nandText, "NAND: %.2f/%.2f MB", usedBytes / 1048576.0d, totalBytes / 1048576.0d);
		addLog("NAND: %llu B free out of %llu B", freeBytes, totalBytes);
	}else{
		generateTextStatic(&nandText, "NAND error");
	}
	
	errCol = C2D_Color32(255, 0, 0, 255);
	clearCol = C2D_Color32(225, 225, 225, 255);
	backCol = C2D_Color32(190, 190, 190, 255);
	greenCol = C2D_Color32(0, 200, 0, 255);
	chargingCol = C2D_Color32(192, 85, 255, 255);
	blackCol = C2D_Color32(0, 0, 0, 255);
	barCol = C2D_Color32(255, 45, 45, 255);
}

void sceneRenderBottom(){
	C2D_DrawText(&exitText, C2D_AtBaseline | C2D_AlignCenter, BOTTOM_SCREEN_WIDTH / 2.0f, BOTTOM_SCREEN_HEIGHT - 5.0, 0.0f, 0.5f, 0.5f);
	
	const float tab = 10.0f;
	const float border = 3.0f;
	const float ysize = 190.0f;
	
	C2D_DrawRectSolid(tab, tab, 0, BOTTOM_SCREEN_WIDTH - tab * 2, ysize, blackCol);
	C2D_DrawRectSolid(tab + border, tab + border, 0, BOTTOM_SCREEN_WIDTH - tab * 2 - border * 2, ysize - border * 2, backCol);
	
	float ypos = tab + border + 2.0f;
	
	for(int i = 0; i < LOG_LINES; i++){
		if(logg[i][0] == '\0')
			continue;
		
		C2D_DrawText(&logText[i], 0, tab + border + 2.0, ypos, 0.0f, 0.41f, 0.41f);
		ypos += 15.0f;
	}
}

void sceneRenderTop(){
	static u8 battery, charging, lastBattery, internet;
	static u64 lastUpdate = 0, lastCharge = 0, lastClock = 0, lastInternet = 0, lastCharging = 0;
	static char clockText[64], wifiText[64], uptimeText[64];
	
	// Clear the dynamic text buffer
	C2D_TextBufClear(dynamicBuffer);
	
	//Update
	u64 now = osGetTime();
	if(now - lastUpdate >= updateTime){
		Result res = MCUHWC_GetBatteryLevel(&battery);
		if(!R_SUCCEEDED(res))
			battery = 255;
		
		res = PTMU_GetBatteryChargeState(&charging);
		
		if(charging && battery != lastBattery){
			addLog("Charged to %d%% after %.2f s", battery, (now - lastCharge) / 1000.0d);
			lastCharge = now;
			lastBattery = battery;
		}
		
		u32 status;
		if(R_SUCCEEDED(ACU_GetWifiStatus(&status))){
			char ssid[33];
			res = ACU_GetSSID(ssid);
			
			if(R_SUCCEEDED(res)){
				snprintf(wifiText, sizeof(wifiText), "Internet: %s", ssid);
				internet = 1;
			}else{
				snprintf(wifiText, sizeof(wifiText), "Internet: Disconected");
				internet = 0;
			}
		}else{
			snprintf(wifiText, sizeof(wifiText), "Internet: Disconected");
			internet = 0;
		}
		
		if(internet != lastInternet){
			addLog("%s", wifiText);
			lastInternet = internet;
		}
		
		if(charging != lastCharging){
			addLog(charging ? "Charging" : "Disconnected");
			lastCharging = charging;
		}
		
		lastUpdate = now;
	}
	
	//Time
	if(now - lastClock >= 900){
		time_t unixTime = time(NULL);
		struct tm* timeStruct = gmtime((const time_t *)&unixTime);

		int hours = timeStruct->tm_hour;
		int minutes = timeStruct->tm_min;
		int seconds = timeStruct->tm_sec;
		int day = timeStruct->tm_mday;
		int month = timeStruct->tm_mon;
		int year = timeStruct->tm_year + 1900;
		
		snprintf(clockText, sizeof(clockText), "%s %02d %s %d %02d:%02d:%02d", weekDays[getDayOfWeek(day, month, year)], day, months[month], year, hours, minutes, seconds);
		
		u64 uptime = svcGetSystemTick();
		
		const int TICKS_PER_SECOND = 268123480;
		u64 totalSeconds = uptime / TICKS_PER_SECOND;
		
		u32 hours2 = totalSeconds / 3600;
		u32 minutes2 = (totalSeconds % 3600) / 60;
		u32 seconds2 = totalSeconds % 60;
		
		snprintf(uptimeText, sizeof(uptimeText), "Uptime: %02lu:%02lu:%02lu", hours2, minutes2, seconds2);
	}
	
	//Display
	if(battery > 100){
		C2D_DrawText(&batteryErrorText, C2D_AlignCenter | C2D_WithColor, TOP_SCREEN_WIDTH / 2.0f, 10.0f, 0.0f, 0.5f, 0.5f, errCol);
	}else{
		drawBar(10.0f, charging ? chargingCol : barCol, battery / 100.0, &batteryText);
	}
	
	//SD and nand
	drawBar(50.0f, barCol, sdFilled, &sdText);
	drawBar(90.0f, barCol, nandFilled, &nandText);
	
	//Internet
	C2D_Text dynText4;
	generateTextDynamic(&dynText4, wifiText);
	if(internet){
		C2D_DrawText(&dynText4, C2D_WithColor, 15.0f, 130.0f, 0.0f, 0.5f, 0.5f, greenCol);
	}else{
		C2D_DrawText(&dynText4, 0, 15.0f, 130.0f, 0.0f, 0.5f, 0.5f);
	}
	
	//Time to update
	C2D_Text dynText2;
	generateTextDynamic(&dynText2, "%.2f / %.2f s", (updateTime - now + lastUpdate) / 1000.0d, updateTime / 1000.0d);
	C2D_DrawText(&dynText2, C2D_AtBaseline, 5.0f, TOP_SCREEN_HEIGHT - 5.0f, 0.0f, 0.4f, 0.4f);
	
	//Clock
	C2D_Text dynText3;
	generateTextDynamic(&dynText3, clockText);
	C2D_DrawText(&dynText3, C2D_AtBaseline | C2D_AlignRight, TOP_SCREEN_WIDTH - 5.0, TOP_SCREEN_HEIGHT - 5.0f, 0.0f, 0.4f, 0.4f);
	
	//Uptime
	C2D_Text dynText5;
	generateTextDynamic(&dynText5, uptimeText);
	C2D_DrawText(&dynText5, 0, 15.0f, 150.0, 0.0f, 0.5f, 0.5f);
}

void sceneExit(){
	// Delete the text buffers
	C2D_TextBufDelete(dynamicBuffer);
	C2D_TextBufDelete(staticBuffer);
	C2D_TextBufDelete(logBuffer);
}

void error(const char* text){
	consoleInit(GFX_BOTTOM, NULL);
	
	printf("\x1b[31m");
	printf(text);
	printf("\nExiting in 5 seconds...\n");
	
	svcSleepThread(5000000000LL);
	
	gfxExit();
}

int main(int argc, char **argv){
	//Initialize the graphics system with the default config
	gfxInitDefault();
	
	Result res = mcuHwcInit();
	if(R_FAILED(res)) {
        error("Failed to initialize MCUHWC");
		return 1;
    }
	
	res = ptmuInit();
	if(R_FAILED(res)) {
        error("Failed to initialize PTMU.");
		return 1;
    }
	
	res = acInit();
	if(R_FAILED(res)){
        error("Failed to initialize ACU.");
		return 1;
    }
	
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	
	C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	
	sceneInit();
	
	KeyRepeat up, down;
	krCreate(&up, KEY_UP, 0.25f, 0.05f);
	krCreate(&down, KEY_DOWN, 0.25f, 0.05f);
	
	while(aptMainLoop()){
		//START
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		
		C2D_TargetClear(top, clearCol);
		C2D_SceneBegin(top);
		
		sceneRenderTop();
		
		if(updateBottom){
			C2D_TargetClear(bottom, clearCol);
			C2D_SceneBegin(bottom);
			
			sceneRenderBottom();
			
			updateBottom = false;
		}
		
		//END
		C3D_FrameEnd(0);
		
		//KEY presses
		hidScanInput();
		u32 kDown = hidKeysDown();
		u32 kHeld = hidKeysHeld();
		if(kDown & KEY_START){
			break; //break in order to return to hbmenu
		}
		
		if(krUpdate(&down, kHeld, kDown)){
			updateTime -= 50;
		}else if(krUpdate(&up, kHeld, kDown)){
			updateTime += 50;
		}
		
		if(updateTime > 1000000){ //Overflow
			updateTime = 0;
		}
		
		svcSleepThread((s64) 1.0d / TARGET_FRAMERATE * 1000000000LL);
	}
	
	mcuHwcExit();
	
	sceneExit();
	
	//Exit the graphics system
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}