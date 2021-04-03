#include <math.h>
#include <windows.h>
#include <windowsx.h>
#include <xinput.h>
#include <dsound.h>
#include "menus.h"
/*General Program State*/
static BOOL g_change;
static BOOL g_locked;
static BOOL g_game;
static BOOL g_paused;
static BOOLEAN g_keys[256];
/*Editor*/
static char g_path[MAX_PATH];
static int g_ctrr;
static int g_selection = 1;
static DWORD CALLBACK(*dyXInputGetState)(DWORD, XINPUT_STATE *) = NULL; 
/*Audio*/
#define WAVE_MUSIC 0
#define WAVE_JUMP 1
#define WAVE_LOST 2
#define WAVE_APPEARS 3
#define WAVE_POWER 4
#define WAVE_COIN 5
#define WAVE_RICOCHET 6
#define WAVE_COUNT 7
static struct IDirectSound *g_dsound;
static struct IDirectSoundBuffer *g_primary;
static IDirectSoundBuffer *g_waves[WAVE_COUNT];
static const WAVEFORMATEX g_format = {
	.wFormatTag = WAVE_FORMAT_PCM,
	.nChannels = 2,
	.nSamplesPerSec = 22050,
	.nAvgBytesPerSec = 22050 * 4,
	.nBlockAlign = 4,
	.wBitsPerSample = 16
};
struct IDirectSoundBuffer *LoadWave(const char *path) {
	if(!g_dsound) {
		return NULL;
	}
	HANDLE fh = CreateFile(path, GENERIC_READ, 0,  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if(fh == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	/*Reader header*/
	struct {
		DWORD idChunk;
		DWORD cbChunk;
		DWORD idFormat;
		DWORD idSubChunk;
		DWORD cbSubChunk;
		/*Identical WAVEFORMATEX if it weren't for cbSize*/
		WORD wFormatTag;
		WORD nChannels;
		DWORD nSamplesPerSec;
		DWORD nAvgBytesPerSec;
		WORD nBlockAlign;
		WORD wBitsPerSample;
		/*Would be were cbSize started*/	
		DWORD idDataChunk;
		DWORD cbData;
	} header;
	DWORD read;
	ReadFile(fh, &header, sizeof(header), &read, NULL); 
	if(read != sizeof(header) ||
			header.idChunk != 0x46464952 || 
			header.idFormat != 0x45564157 ||
			header.idSubChunk != 0x20746D66 ||
			header.wFormatTag != g_format.wFormatTag ||
		    	header.nSamplesPerSec != g_format.nSamplesPerSec ||
			header.wBitsPerSample != g_format.wBitsPerSample || 
			header.idDataChunk != 0x61746164) {
		goto header_invalid;
	}
	/*Create new buffer*/
	WAVEFORMATEX format = {
		.wFormatTag = header.wFormatTag,
		.nChannels = header.nChannels,
		.nSamplesPerSec = header.nSamplesPerSec,
		.nAvgBytesPerSec = header.nAvgBytesPerSec,
		.nBlockAlign = header.nBlockAlign,
		.wBitsPerSample = header.wBitsPerSample,
	};
	DSBUFFERDESC desc = {
		.dwSize = sizeof(DSBUFFERDESC),
		.dwBufferBytes = header.cbData,
		.lpwfxFormat = &format,
		.dwFlags = DSBCAPS_CTRLFREQUENCY
	};
	struct IDirectSoundBuffer *secondary;
	if(FAILED(IDirectSound_CreateSoundBuffer(g_dsound, &desc, &secondary, NULL))) {
		goto header_invalid;	
	}
	/*Write data to new buffer*/
	void *buf;
	DWORD cbBuf;
	if(FAILED(IDirectSoundBuffer_Lock(secondary, 0, header.cbData, &buf, &cbBuf, NULL, 0, 0))) {
		goto secondary_invalid;
	}
	ReadFile(fh, buf, header.cbData, &read, NULL);
	if(read != header.cbData ||
			FAILED(IDirectSoundBuffer_Unlock(secondary, buf, cbBuf, NULL, 0))) {
		goto secondary_invalid;
	}
	/*Resource handling*/
	CloseHandle(fh);
	return secondary;
secondary_invalid:
	IDirectSoundBuffer_Release(secondary);
header_invalid:
	CloseHandle(fh);
	return NULL;
}
static void PlayWave(struct IDirectSoundBuffer *secondary) {
	if(secondary) {
		IDirectSoundBuffer_SetCurrentPosition(secondary, 0);
		IDirectSoundBuffer_Play(secondary, 0, 0, 0);
	}
}
static void StopWave(struct IDirectSoundBuffer *secondary) {
	if(secondary) {
		IDirectSoundBuffer_Stop(secondary);
	}
}
/*Mushroom*/
#define MUSHROOM_LEFT_X 307
#define MUSHROOM_RIGHT_X 290
#define MUSHROOM_Y 0
#define DINOSAUR_LEFT_X 272
#define DINOSAUR_RIGHT_X 291
#define DINOSAUR_Y 289
/*Room*/
#define BUFFER_WIDTH 256
#define BUFFER_HEIGHT 224 
#define ALIGN_TO_TILE(component) ((int) (component) & ~15)
#define WALKING_SPEED 1.0f
#define RUNNING_SPEED 2.0f
#define SPRINTING_SPEED 3.0f
struct IPoint {
	int x, y;
};
struct FPoint {
	float x, y;
};
static int g_entitiesUsed;
static int g_effectsUsed;
static int g_ascendsUsed;
struct Ascend {
	struct FPoint pos;
	struct IPoint sprite;
	float yVel;
	float yVelMax;
	float yAcc;
} g_ascends[16];
#define ENTITY_MUSHROOM 0
#define ENTITY_DINOSAUR 1
struct Entity {
	int type;
	int tick;
	struct FPoint pos;
	struct FPoint vel;
} static g_entities[16]; 
struct Effect {
	struct IPoint pos;
	int tick;
} static g_effects[16]; 
struct {
	struct FPoint pos; /*Fixed Point*/
	struct IPoint sprite;
	struct FPoint vel; /*Fixed Point*/
	int tick;
	int state;
} static g_player;
struct {
	struct FPoint pos;
	float offset;
	float focus;
} static g_camera;
struct {
	struct IPoint pos;
	BOOL exists;
} static g_place;
struct {
	int width;
	int height;
	BYTE tiles[448][512];
} static g_room;
static unsigned g_tick;
/*Player State*/
#define PLAYER_STATE_FALLING 1
#define PLAYER_STATE_JUMPING 2
#define PLAYER_STATE_SMALL 4
#define PLAYER_STATE_DEAD 8
#define PLAYER_STATE_TRANSITION 32
/*Tile Properties*/
#define TILE_PROP_LEFT_SOLID 1
#define TILE_PROP_RIGHT_SOLID 2
#define TILE_PROP_TOP_SOLID 4
#define TILE_PROP_BOTTOM_SOLID 8
#define TILE_PROP_LOOP_FOUR 16
/*Combinations*/
#define TILE_PROPS_HORZ_SOLID (TILE_PROP_LEFT_SOLID | TILE_PROP_RIGHT_SOLID)
#define TILE_PROPS_VERT_SOLID (TILE_PROP_TOP_SOLID | TILE_PROP_BOTTOM_SOLID)
#define TILE_PROPS_FULL_SOLID (TILE_PROPS_HORZ_SOLID | TILE_PROPS_VERT_SOLID)
/*Tile Properties array*/
#define TILE_COUNT 16
#define SELECT_WIDTH ((TILE_COUNT - 1) << 4)
#define SELECT_HEIGHT 16
struct {
	struct IPoint sprite;
	int prop;
} static const g_tiles[256] = {
	{{0, 339}, 0},
	{{34, 102}, TILE_PROP_TOP_SOLID},
	{{34, 119}, 0},
	{{0, 102}, TILE_PROP_TOP_SOLID},
	{{0, 119}, 0},
	{{68, 102}, TILE_PROP_TOP_SOLID},
	{{68, 119}, 0},
	{{17, 102}, TILE_PROP_LEFT_SOLID | TILE_PROP_TOP_SOLID},
	{{17, 119}, TILE_PROP_LEFT_SOLID},
	{{51, 102}, TILE_PROP_RIGHT_SOLID | TILE_PROP_TOP_SOLID},
	{{51, 119}, TILE_PROP_RIGHT_SOLID},
#define TILE_QUESTION 11
	{{17, 17}, TILE_PROPS_FULL_SOLID | TILE_PROP_LOOP_FOUR},
	{{17, 136}, 0},
	{{51, 136}, 0},
#define TILE_ITEM_BLOCK 14
	{{34, 68}, TILE_PROPS_FULL_SOLID}, 
#define TILE_COIN 15
	{{290, 17}, TILE_PROP_LOOP_FOUR},
#define TILE_USED_QUESTION 252
	[252] = {{0, 51}, TILE_PROPS_FULL_SOLID},
#define TILE_USED_COIN 253 
	[253] = {{0, 339}, 0},
#define TILE_USED_ITEM_BLOCK 254 
	[254] = {{0, 51}, TILE_PROPS_FULL_SOLID},
#define TILE_INVISIBLE_SOLID 255
	[255] = {{0, 339}, TILE_PROPS_FULL_SOLID}
};
static void ResetTile(BYTE *tile) {
	switch(*tile) {
	case TILE_USED_QUESTION:
		*tile = TILE_QUESTION;
		break;
	case TILE_USED_ITEM_BLOCK:
		*tile = TILE_ITEM_BLOCK;
		break;
	case TILE_USED_COIN:
		*tile = TILE_COIN;
		break;
	}
}
static void ResetRoom(void) {
	g_player.pos.x = 16.0f;
	g_player.pos.y = 163.0f;
	g_player.sprite.x = 206;
	g_player.sprite.y = 74;
	g_player.vel.x = 0.0f;
	g_player.vel.y = 0.0f;
	g_player.tick = 0;
	g_player.state = 0;
	g_camera.pos.x = 0.0f;
	g_camera.pos.y = 0.0f;
	g_camera.focus = 0.0f;
	g_camera.offset = 0.0f;
	g_entitiesUsed = 0;
	g_effectsUsed = 0;
	g_tick = 0;
	for(int y = 0; y < g_room.height; y++) {
		for(int x = 0; x < g_room.width; x++) {
			ResetTile(&g_room.tiles[y][x]);
		}
	}
}
static void InitialzeRoomDefault(void) {
	g_game = FALSE;
	g_paused = FALSE;
	g_room.width = 16;
	g_room.height = 14;
	memset(g_room.tiles[ 0], 0, g_room.width);
	memset(g_room.tiles[ 1], 0, g_room.width);
	memset(g_room.tiles[ 2], 0, g_room.width);
	memset(g_room.tiles[ 3], 0, g_room.width);
	memset(g_room.tiles[ 4], 0, g_room.width);
	memset(g_room.tiles[ 5], 0, g_room.width);
	memset(g_room.tiles[ 6], 0, g_room.width);
	memset(g_room.tiles[ 7], 0, g_room.width);
	memset(g_room.tiles[ 8], 0, g_room.width);
	memset(g_room.tiles[ 9], 0, g_room.width);
	memset(g_room.tiles[10], 0, g_room.width);
	memset(g_room.tiles[11], 0, g_room.width);
	memset(g_room.tiles[12], 1, g_room.width);
	memset(g_room.tiles[13], 2, g_room.width);
	ResetRoom();
}
static BOOL IsPlayerFacingLeft(void) {
	return g_player.sprite.x < 196;
}
static void SetPlayerToIdleAnimation(void) {
	g_player.tick = 0;
	if(IsPlayerFacingLeft()) {
		g_player.sprite.x = 166;
	} else {
		g_player.sprite.x = 206;
	}
	if(g_player.state & PLAYER_STATE_SMALL) {
		g_player.sprite.y = -10;
	} else {
		g_player.sprite.y = 74;
	}
}
static void AnimatePlayerWalkingLeft(void) {
	if(g_player.vel.x > -SPRINTING_SPEED) {
		if(g_player.tick++ == 0) {
			switch(g_player.sprite.x) {
			case 46:
				g_player.sprite.x = 6;
				break;
			case 166:
				g_player.sprite.x = 46;
				break;
			default:
				g_player.sprite.x = 166;
			}
			g_player.sprite.y = g_player.state & PLAYER_STATE_SMALL ? -10 : 74;
		} else if(g_player.tick > (g_player.vel.x <= -RUNNING_SPEED ? 2 : 5)) {
			g_player.tick = 0;
		}
	} else if(g_player.tick++ == 0) {
		switch(g_player.sprite.x) {
		case 126:
			g_player.sprite.x = 86;
			break;
		case 166:
			g_player.sprite.x = 126;
			break;
		default:
			g_player.sprite.x = 166;
		}
		g_player.sprite.y = g_player.state & PLAYER_STATE_SMALL ? 786 : 154;
	} else if(g_player.tick > 1) {
		g_player.tick = 0;
	}
}
static void AnimatePlayerWalkingRight(void) {
	if(g_player.vel.x < SPRINTING_SPEED) {
		if(g_player.tick++ == 0) {
			switch(g_player.sprite.x) {
			case 206:
				g_player.sprite.x = 326;
				break;
			case 326:
				g_player.sprite.x = 366;
				break;
			default:
				g_player.sprite.x = 206;
			}
			g_player.sprite.y = g_player.state & PLAYER_STATE_SMALL ? -10 : 74;
		} else if(g_player.tick > (g_player.vel.x >= RUNNING_SPEED ? 2 : 5)) {
			g_player.tick = 0;
		}
	} else if(g_player.tick++ == 0) {
		switch(g_player.sprite.x) {
		case 206:
			g_player.sprite.x = 246;
			break;
		case 246:
			g_player.sprite.x = 286;
			break;
		default:
			g_player.sprite.x = 206;
		}
		g_player.sprite.y = g_player.state & PLAYER_STATE_SMALL ? 786 : 154;
	} else if(g_player.tick > 1) {
		g_player.tick = 0;
	}
}
static void SetPlayerStateToIdle(void) {
	if(!(g_player.state & (PLAYER_STATE_JUMPING | PLAYER_STATE_FALLING))) {
		if(g_player.vel.x < 0.0f) {
			if(g_player.vel.x < -0.25f) {
				g_player.vel.x += 0.25f;
				AnimatePlayerWalkingLeft();
			} else {
				g_player.vel.x = 0.0f;
			}
		} else if(g_player.vel.x > 0.0f) {
			if(g_player.vel.x > 0.25f) {
				g_player.vel.x -= 0.25f;
				AnimatePlayerWalkingRight();
			} else {
				g_player.vel.x = 0.0f;
			}
		} else {
			SetPlayerToIdleAnimation();
		}
		g_player.state &= ~(PLAYER_STATE_FALLING | PLAYER_STATE_JUMPING);
	}
}
static BYTE *TileFromCom(int xCom, int yCom) {
	return yCom >= 0 && yCom < g_room.height ? &g_room.tiles[yCom][xCom] : (BYTE *) ""; 
}
struct TileTriple {
	BYTE *head;
	BYTE *body;
	BYTE *feet;
};
static struct TileTriple TileTripleHorz(struct FPoint pos, struct FPoint vel) {
	struct TileTriple triple;
	if(g_player.vel.x == 0) {
		triple.head = (BYTE *) "";
		triple.body = (BYTE *) "";
		triple.feet = (BYTE *) "";
		return triple; 
	}
	int xOff, prop;
	if(g_player.vel.x < 0.0f) {
		xOff = 5;
		prop = TILE_PROP_RIGHT_SOLID;
	} else {
		xOff = 13;
		prop = TILE_PROP_LEFT_SOLID;
	}
	/*GetHozrTiles*/
	int yPos = g_player.pos.y;
	int xCom = (int) g_player.pos.x + xOff >> 4;
	if(g_player.state & PLAYER_STATE_SMALL) {
		triple.head = TileFromCom(xCom, yPos + 18 >> 4);
		triple.body = triple.head;
	} else {	
		triple.head = TileFromCom(xCom, yPos + 8 >> 4);
		triple.body = TileFromCom(xCom, (yPos >> 4) + 1);
	}
	triple.feet = (yPos & 15) > 4 ? TileFromCom(xCom, yPos + 30 >> 4) : (BYTE *) "";
	/*HandleSolidHorzTile*/
	if((g_tiles[*triple.head].prop | g_tiles[*triple.body].prop | g_tiles[*triple.feet].prop) & prop) {
		g_player.vel.x = 0.0f;
		g_player.pos.x = ALIGN_TO_TILE(g_player.pos.x) + 16.0f - xOff;
	}
	return triple;
}
struct TilePair {
	BYTE *left;
	BYTE *right;
};
static struct TilePair TilePairBelow(struct FPoint pos, struct FPoint vel, int left, int right, int yOff) {
	int yCom = ((int) pos.y >> 4) + yOff;
	struct TilePair pair; 
	if(yCom >= 0 && yCom < g_room.height && (vel.y <= 0.0f || (int) pos.y >> 4 != (int) (pos.y - vel.y) >> 4)) {
		pair.left = &g_room.tiles[yCom][((int) pos.x + left) >> 4];
		pair.right = &g_room.tiles[yCom][((int) pos.x + right) >> 4];
	} else {
		pair.left = (BYTE *) ""; 
		pair.right = (BYTE *) "";
	}
	return pair;
}
struct IPoint TileGetPoint(BYTE *tile) {
	int off = tile - g_room.tiles[0];
	struct IPoint pt = {(off & 511) << 4, (off >> 9) << 4};
	return pt;	
}
static void CollectCoin(BYTE *coin) {
	*coin = TILE_USED_COIN;
	struct IPoint pos = TileGetPoint(coin);	
	if(g_effectsUsed < _countof(g_effects)) {
		g_effects[g_effectsUsed].pos = pos;
		g_effects[g_effectsUsed].tick = 35;
		g_effectsUsed++;
	}
	PlayWave(g_waves[WAVE_COIN]);
}
static BOOL YComInRoomHasProp(int yCom, BYTE *colPtr, int prop) {
	return yCom >= 0 && yCom < g_room.height &&
			g_tiles[colPtr[yCom << 9]].prop & prop;
}
static BOOL MushroomSolidHorzTileOccurred(int xPos, int yPos, int xDsp, int prop) {
	return (YComInRoomHasProp(yPos >> 4, &g_room.tiles[0][xPos + xDsp >> 4], prop) ||
			YComInRoomHasProp((yPos + 15) >> 4, &g_room.tiles[0][xPos + xDsp >> 4], prop));
}
static void SetPlayerSpriteToFallingLeft(void) {
	if(g_player.vel.x < SPRINTING_SPEED) {
		g_player.sprite.x = 126;
		g_player.sprite.y = g_player.state & PLAYER_STATE_SMALL ? 31 : 114; 
	} else {
		g_player.sprite.x = 46;
		g_player.sprite.y = g_player.state & PLAYER_STATE_SMALL ? 786 : 154;
	}
}
static void SetPlayerSpriteToFallingRight(void) {
	if(g_player.vel.x < SPRINTING_SPEED) {
		g_player.sprite.x = 246;
		g_player.sprite.y = g_player.state & PLAYER_STATE_SMALL ? 31 : 114; 
	} else {
		g_player.sprite.x = 326;
		g_player.sprite.y = g_player.state & PLAYER_STATE_SMALL ? 786 : 154;
	}
}
static void SetPlayerStateToFalling(void) {
	g_player.state &= ~PLAYER_STATE_JUMPING;
	g_player.state |= PLAYER_STATE_FALLING;
	if(IsPlayerFacingLeft()) {
		SetPlayerSpriteToFallingLeft();
	} else {
		SetPlayerSpriteToFallingRight();
	}
}
static void SetPlayerSpriteToJumpLeft(void) {
	if(g_player.vel.x > -SPRINTING_SPEED) {
		g_player.sprite.x = 166;
		g_player.sprite.y = g_player.state & PLAYER_STATE_SMALL ? 31 : 114; 
	} else {
		g_player.sprite.x = 46;
		g_player.sprite.y = g_player.state & PLAYER_STATE_SMALL ? 786 : 154;
	}
}
static void SetPlayerSpriteToJumpRight(void) {
	if(g_player.vel.x < SPRINTING_SPEED) {
		g_player.sprite.x = 206;
		g_player.sprite.y = g_player.state & PLAYER_STATE_SMALL ? 31 : 114; 
	} else {
		g_player.sprite.x = 326;
		g_player.sprite.y = g_player.state & PLAYER_STATE_SMALL ? 786 : 154;
	}
}
/*Rendering*/
static LONGLONG g_fps = 60;
struct BitmapAlpha {
	HBITMAP color;
	HBITMAP mask;
};
static HBITMAP g_hbmBuffer;
static HBITMAP g_hbmScratch;
static HBITMAP g_hbmSelect;
static struct BitmapAlpha g_baMario;
static struct BitmapAlpha g_baTiles;
static struct BitmapAlpha g_baBackgrounds;
static void RenderToScreen(HDC hdcDst) {
	HDC hdcSrc = CreateCompatibleDC(hdcDst);
	HBITMAP hbmOld = SelectObject(hdcSrc, g_hbmBuffer);
	StretchBlt(hdcDst, 0, 0, BUFFER_WIDTH * g_ctrr, BUFFER_HEIGHT * g_ctrr, 
			hdcSrc, 0, 0, BUFFER_WIDTH, BUFFER_HEIGHT, 
			SRCCOPY);
	SelectObject(hdcSrc, g_hbmSelect); 
	StretchBlt(hdcDst, BUFFER_WIDTH * g_ctrr, 0, SELECT_WIDTH * g_ctrr, SELECT_HEIGHT * g_ctrr,
			hdcSrc, 0, 0, SELECT_WIDTH, SELECT_HEIGHT,
			SRCCOPY);	   
	SelectObject(hdcSrc, hbmOld);
	DeleteDC(hdcSrc);
}
static HBITMAP RenderAlpha(HDC hdcDst, int xDst, int yDst, int width, int height,
		HDC hdcSrc, int xSrc, int ySrc, 
		struct BitmapAlpha *bm) {
	HBITMAP hbmOld = SelectObject(hdcSrc, bm->mask);
	BitBlt(hdcDst, xDst, yDst, width, height, hdcSrc, xSrc, ySrc, SRCAND);
	SelectObject(hdcSrc, bm->color);
	BitBlt(hdcDst, xDst, yDst, width, height, hdcSrc, xSrc, ySrc, SRCPAINT);
	return hbmOld;
}
static void RenderObject(HDC hdcDst, float xPos, float yPos, int width, int height, 
		HDC hdcSrc, int xSrc, int ySrc, struct BitmapAlpha *bm) {
	RenderAlpha(hdcDst, xPos - g_camera.pos.x, yPos - (int) g_camera.pos.y, width, height, 
			hdcSrc, xSrc, ySrc, bm);	
}
static void RenderSelector(void) {
	HDC hdcDst = CreateCompatibleDC(NULL);
	HDC hdcSrc = CreateCompatibleDC(NULL); 
	HBITMAP hbmDst = SelectObject(hdcDst, g_hbmSelect);
	HBITMAP hbmSrc = SelectObject(hdcSrc, g_baTiles.color);
	for(int i = 1; i < TILE_COUNT; i++) {
		BitBlt(hdcDst, (i - 1) << 4, 0, 16, 16, hdcSrc, g_tiles[i].sprite.x, g_tiles[i].sprite.y, SRCCOPY); 
	}
	RenderAlpha(hdcDst, (g_selection - 1) << 4, 0, 16, 16, hdcSrc, 255, 272, &g_baTiles);
	SelectObject(hdcSrc, hbmSrc);
	SelectObject(hdcDst, hbmDst);
	DeleteDC(hdcSrc);
	DeleteDC(hdcDst);
}
static void RenderStar(HDC hdcDst, int xDst, int yDst, int side, HDC hdcSrc) {
	int dsp = 2 - side / 2;
	RenderObject(hdcDst, xDst + dsp, yDst + (dsp == 2 ? 3 : dsp), side, side, 
			hdcSrc, 272 + dsp, 272 + dsp, &g_baTiles); 
}
/*Uses g_room data in order to update buffer that is rendered to the screen*/
static void RenderBuffer(void) {
	HDC hdcDst = CreateCompatibleDC(NULL);
	HDC hdcSrc = CreateCompatibleDC(NULL);
	HBITMAP hbmDst = SelectObject(hdcDst, g_hbmBuffer);
	/*Clear with background color*/
	HBRUSH hbrDst = SelectObject(hdcDst, GetStockObject(DC_BRUSH));
	SetDCBrushColor(hdcDst, RGB(0, 96, 184));
	PatBlt(hdcDst, 0, 0, BUFFER_WIDTH, BUFFER_HEIGHT, PATCOPY);
	SelectObject(hdcDst, hbrDst);
	/*Render background image*/
	int xOff = (int) g_camera.pos.x % 512;
	int yPosSrc = 646 - g_camera.pos.y;
	int width = 512 - xOff;
	HBITMAP hbmSrc = RenderAlpha(hdcDst, 0, 0, width, 224, 
			hdcSrc, 516 + xOff, yPosSrc, 
			&g_baBackgrounds);
	RenderAlpha(hdcDst, width, 0, 256, 224, 
			hdcSrc, 516, yPosSrc,
			&g_baBackgrounds);
	/*Get Integer position of camera*/
	int xCam = ceilf(g_camera.pos.x);
	int yCam = g_camera.pos.y;
	/*Determine starting pos of rendering on x-direction*/
	int xBegin = -(xCam & 15);
	int xEnd = xBegin + BUFFER_WIDTH;
	int tcxJump = 512 - BUFFER_WIDTH / 16;
	if(xBegin) {
		xEnd += 16;
		tcxJump--;		
	} 
	const BYTE *tile = &g_room.tiles[0][xCam >> 4];
	/*RenderBottomLayerEntity*/
	for(int i = 0; i < g_entitiesUsed; i++) {
		if(g_entities[i].tick > 0 && g_entities[i].type == ENTITY_MUSHROOM) {
			RenderObject(hdcDst, g_entities[i].pos.x, g_entities[i].pos.y, 16, 16, 
					hdcSrc, g_entities[i].vel.x < 0.0f ? MUSHROOM_LEFT_X : MUSHROOM_RIGHT_X, MUSHROOM_Y, 
					&g_baTiles);
		}
	}
	/*Render Loop*/
	int yEnd = BUFFER_HEIGHT + yCam; 
	for(int yPos = 0; yPos < yEnd; yPos += 16) {
		for(int xPos = xBegin; xPos < xEnd; xPos += 16) {
			if(*tile > 0) {
				int xDst = g_tiles[*tile].sprite.x;
				if(g_tiles[*tile].prop & TILE_PROP_LOOP_FOUR) {
					xDst += (g_tick >> 3 & 3) * 17;
				}
				RenderAlpha(hdcDst, xPos, yPos, 16, 16,
						hdcSrc, xDst, g_tiles[*tile].sprite.y,
						&g_baTiles); 
			}
			tile++;
		}
		tile += tcxJump;
	}
	/*RenderPlayer*/
	RenderObject(hdcDst, g_player.pos.x, g_player.pos.y, 20, 32, 
			hdcSrc, g_player.sprite.x, g_player.sprite.y, &g_baMario);
	/*RenderEntityTopLayer*/
	for(int i = 0; i < g_entitiesUsed; i++) {
		struct Entity *entity = &g_entities[i];
		int xSrc, ySrc;
		int width, height;
		switch(entity->type) {
		case ENTITY_MUSHROOM:
			if(entity->tick <= 0) {
				xSrc = entity->vel.x < 0.0f ? MUSHROOM_LEFT_X : MUSHROOM_RIGHT_X;
				ySrc = MUSHROOM_Y;
				width = 16;
				height = 16;
				RenderObject(hdcDst, entity->pos.x, entity->pos.y, width, height, 
				             hdcSrc, xSrc, ySrc, &g_baTiles);
			}
			break;
		case ENTITY_DINOSAUR:	
			xSrc = entity->vel.x < 0.0f ? DINOSAUR_LEFT_X : DINOSAUR_RIGHT_X; 
			ySrc = entity->tick >> 3 & 1 ? DINOSAUR_Y : DINOSAUR_Y + 32;
			width = 21;
			height = 32;
			RenderObject(hdcDst, entity->pos.x, entity->pos.y, width, height, 
				hdcSrc, xSrc, ySrc, 
				&g_baTiles);
			break;
		}
	}
	/*RenderAscend*/
	for(int i = 0; i < g_ascendsUsed; i++) {
		struct Ascend *ascend = &g_ascends[i];
		int xDst = ascend->sprite.x;
		if(ascend->sprite.x == g_tiles[TILE_COIN].sprite.x) {
			xDst += (g_tick >> 3 & 3) * 17;
		}
		RenderObject(hdcDst, ascend->pos.x, ascend->pos.y, 16, 16, 
				hdcSrc, xDst, ascend->sprite.y, &g_baTiles);
	}
	/*RenderEffects*/
	for(int i = 0; i < g_effectsUsed; i++) {
		struct IPoint pos = g_effects[i].pos;
		if(g_effects[i].tick < 35) {
			if(g_effects[i].tick < 12) {
				/*Small right star*/
				RenderStar(hdcDst, pos.x + 11, pos.y + 6, 1, hdcSrc);	
			} else if(g_effects[i].tick < 20) {
				/*Small left star*/	
				RenderStar(hdcDst, pos.x + 1, pos.y + 6, 1, hdcSrc);
				/*Medium right star*/
				RenderStar(hdcDst, pos.x + 11, pos.y + 6, 3, hdcSrc);
			} else if(g_effects[i].tick < 28) {
				/*Medium left star*/
				RenderStar(hdcDst, pos.x + 1, pos.y + 6, 3, hdcSrc);
				if(g_effects[i].tick < 27) {
					/*Large right start*/
					RenderStar(hdcDst, pos.x + 11, pos.y + 6, 5, hdcSrc);
				}
			} else {
				/*Large left start*/
				RenderStar(hdcDst, pos.x + 1, pos.y + 6, 5, hdcSrc);
			}
			if(g_effects[i].tick >= 4) {
				if(g_effects[i].tick < 8) {
					/*Small top star*/	
					RenderStar(hdcDst, pos.x + 6, pos.y - 2, 1, hdcSrc); 
				} else if(g_effects[i].tick < 16) {
					/*Medium top star*/
					RenderStar(hdcDst, pos.x + 6, pos.y - 2, 3, hdcSrc);
					/*Small bottom star*/
					RenderStar(hdcDst, pos.x + 6, pos.y + 14, 1, hdcSrc); 
				} else if(g_effects[i].tick < 24) {
					/*Medium bottom star*/
					RenderStar(hdcDst, pos.x + 6, pos.y + 14, 3, hdcSrc); 
					if(g_effects[i].tick < 23) {
						/*Big top star*/
						RenderStar(hdcDst, pos.x + 6, pos.y - 2, 5, hdcSrc); 
					}
				} else if(g_effects[i].tick < 31) {
					RenderStar(hdcDst, pos.x + 6, pos.y + 14, 5, hdcSrc); 
				}
			}
		}
	}
	/*RenderPlacer*/
	if(g_place.exists) {
		RenderObject(hdcDst, g_place.pos.x, g_place.pos.y, 16, 16, hdcSrc, 255, 272, &g_baTiles);
	}
	/*RenderDeath*/
	if(g_player.state & PLAYER_STATE_DEAD && g_player.tick < 120) {
		SelectObject(hdcSrc, g_hbmScratch);
		BLENDFUNCTION ftn;
		ftn.BlendOp = AC_SRC_OVER;
		ftn.BlendFlags = 0;
		ftn.SourceConstantAlpha = 255 - g_player.tick * 255 / 120;
		ftn.AlphaFormat = AC_SRC_ALPHA;
		AlphaBlend(hdcDst, 0, 0, BUFFER_WIDTH, BUFFER_HEIGHT,
				hdcSrc, 0, 0, 1, 1, ftn);
	} 
	/*Clean Up*/
	SelectObject(hdcSrc, hbmSrc);
	SelectObject(hdcDst, hbmDst);
	DeleteDC(hdcSrc);
	DeleteDC(hdcDst);
}
/**/
static HWND g_hWnd;
static HMENU g_menu;
static void UpdateAndInvalidate(void) {
	if(!g_game || g_paused) {
		RenderBuffer();
		InvalidateRect(g_hWnd, NULL, FALSE);
	}
}
static BOOL WndContinue(void) {
	return !g_change || MessageBox(g_hWnd, "Unsaved changes will be lost", 
			"Warning", MB_ICONEXCLAMATION | MB_OKCANCEL) == IDOK;
}
static void PlaceTile(int xPlace, int yPlace, int tile, BOOL mustRender) {
	BYTE *location = &g_room.tiles[yPlace >> 4][xPlace >> 4];
	if(*location != tile) {
		g_change = TRUE;
		*location = tile;
		UpdateAndInvalidate();
	} else if(mustRender) {
		UpdateAndInvalidate();
	}
}
static CALLBACK INT_PTR DlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch(uMsg) {
	case WM_INITDIALOG:
		SetDlgItemInt(hWnd, ID_WIDTH, g_room.width, FALSE);
		SetDlgItemInt(hWnd, ID_HEIGHT, g_room.height, FALSE);
		return TRUE;
	case WM_COMMAND:
		if(wParam == IDOK) {
			int width = GetDlgItemInt(hWnd, ID_WIDTH, NULL, FALSE);
			int height = GetDlgItemInt(hWnd, ID_HEIGHT, NULL, FALSE);
			if(width < 16) {
				MessageBox(hWnd, "Width must be at least 16", "Error", MB_ICONERROR);
			} else if(width > 512) {
				MessageBox(hWnd, "Width must be at most 512", "Error", MB_ICONERROR);
			}
			if(height < 14) {
				MessageBox(hWnd, "Height must be at least 14", "Error", MB_ICONERROR);
			} else if(height > 448) {
				MessageBox(hWnd, "Height must be at most 448", "Error", MB_ICONERROR);
			} else if(width >= 16 && width <= 512) {
				g_room.width = width;
				g_room.height = height;
				EndDialog(hWnd, 0);
			}
		} else if(wParam == IDCANCEL) {
			EndDialog(hWnd, 0);	
		}
		return TRUE;
	}
	return FALSE;
}
static BOOL MapSave(const char *path) {
	HANDLE fh = CreateFile(path, GENERIC_WRITE, 0,  NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if(fh != INVALID_HANDLE_VALUE) {	
		int tileCount = g_room.width * g_room.height;
		int size = sizeof(WORD) * 2 + tileCount;
		struct RoomFileData {
			WORD width;
			WORD height;
			BYTE tiles[];
		} *data = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		DWORD write = 0;
		if(data) {
			for(int row = 0; row < g_room.height; row++) {
				memcpy(&data->tiles[row * g_room.width], g_room.tiles[row], g_room.width);
			}
			for(int i = 0; i < tileCount; i++) {
				ResetTile(&data->tiles[i]);
			}	
			WriteFile(fh, data, size, &write, NULL);
			VirtualFree(data, 0, MEM_RELEASE);
		}
		CloseHandle(fh);
		if(write == size) {
			g_change = FALSE;
			return TRUE;
		}
	}
	MessageBox(g_hWnd, "Map could not be saved", "Error", MB_ICONERROR);
	return FALSE;
}
static int IClamp(int val, int lower, int upper) {
		if(val < lower) {
			return lower;
		}
		if(val > upper) {
			return upper;
		}
		return val;
}
static int GetPosFromCenterFocus(int xPos) {
	return IClamp((xPos & ~15) - 128, 0, (g_room.width << 4) - BUFFER_WIDTH);
}
static BOOL MapLoad(const char *path) {
	HANDLE fh = CreateFile(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if(fh != INVALID_HANDLE_VALUE) {
		struct RoomFileData {
			WORD width;
			WORD height;
			BYTE tiles[448 * 512];
		} *data = VirtualAlloc(NULL, sizeof(*data), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if(data) {
			DWORD read;
			ReadFile(fh, data, sizeof(*data), &read, NULL);
			CloseHandle(fh);
			g_room.width = IClamp(data->width, 16, 512);
			g_room.height = IClamp(data->height, 14, 448);
			for(int row = 0; row < g_room.height; row++) {
				memcpy(g_room.tiles[row], &data->tiles[row * g_room.width], g_room.width);
			}
			VirtualFree(data, 0, MEM_RELEASE);
			strcpy(g_path, path);
			g_change = FALSE;
			return TRUE;
		}
		CloseHandle(fh);
	}
	MessageBox(g_hWnd, "Map could not be loaded", "Error", MB_ICONERROR);
	return FALSE;
}
static void UpdateSelection(int selection) {
	g_selection = selection;
	RenderSelector();
	if(!g_game || g_paused) {
		InvalidateRect(g_hWnd, NULL, FALSE);
	}
}
static void PauseGame(void) {
	if(g_paused) {
		g_paused = FALSE;
		for(int i = 0; i < WAVE_COUNT; i++) {
			DWORD playCursor;
			if(g_waves[i] && SUCCEEDED(IDirectSoundBuffer_GetCurrentPosition(
					g_waves[i], &playCursor, NULL)) && playCursor) {
				IDirectSoundBuffer_Play(g_waves[i], 0, 0, 0);
			}
		}	
		CheckMenuItem(g_menu, ID_PAUSED, MF_UNCHECKED);
	} else {
		g_paused = TRUE;
		for(int i = 0; i < WAVE_COUNT; i++) {
			DWORD status = 0;
			if(g_waves[i]) {
				IDirectSoundBuffer_GetStatus(g_waves[i], &status);
			       	if(!(status & DSBSTATUS_PLAYING)) {
					IDirectSoundBuffer_SetCurrentPosition(g_waves[i], 0);
				}
				IDirectSoundBuffer_Stop(g_waves[i]);
			}
		}
		CheckMenuItem(g_menu, ID_PAUSED, MF_CHECKED);
	}
}
static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	static LONG volume = DSBVOLUME_MIN;
	OPENFILENAME ofn;
	PAINTSTRUCT paint;
	char path[MAX_PATH];
	int xPos, yPos;
	switch(uMsg) {
	case WM_ACTIVATE:
		if(wParam == WA_INACTIVE) {
			ZeroMemory(g_keys, sizeof(g_keys));
		}
		return 0;
	case WM_CLOSE:
		if(WndContinue()) {
			PostQuitMessage(0);
		}
		return 0;
	case WM_PAINT:
		RenderToScreen(BeginPaint(hWnd, &paint));
		EndPaint(hWnd, &paint);
		return 0;
	case WM_KEYDOWN:
		/*g_keys is only updated if key is pressed not held*/
		if(!(lParam & 0x40000000)) {
			g_keys[wParam] = TRUE;
		}
		/*Moves placer*/
		if(g_place.exists) {
			BOOL move = FALSE;
			if(g_keys['A']) {
				if(g_place.pos.x > 0) {
					g_place.pos.x -= 16; 
					move = TRUE;
					if(!g_game && !g_locked && g_camera.pos.x > 0) {
						g_camera.pos.x = GetPosFromCenterFocus(g_place.pos.x);
					}
				}
			} else if(g_keys['D']) {
				if(g_place.pos.x < (g_room.width - 1) << 4) {
					g_place.pos.x += 16; 
					move = TRUE;
					if(!g_game && !g_locked && 
							((int) g_camera.pos.x >> 4) + 16 < g_room.width) {
						g_camera.pos.x = GetPosFromCenterFocus(g_place.pos.x);
					}
				}
			} else if(g_keys['S']) {
				if(g_place.pos.y < (g_room.height - 1) << 4) {
					g_place.pos.y += 16; 
					move = TRUE;
				}
			} else if(g_keys['W']) {
				if(g_place.pos.y > 0) {
					g_place.pos.y -= 16; 
					move = TRUE;
				}
			}
			if(g_keys[VK_RETURN]) {
				PlaceTile(g_place.pos.x, g_place.pos.y, g_selection, move);
			} else if(g_keys[VK_BACK]) {
				PlaceTile(g_place.pos.x, g_place.pos.y, 0, move);	
			} else if(move) {
				UpdateAndInvalidate();
			}
		}
		/*Selection mover*/
		if(g_keys['H']) {
			if(--g_selection < 1) {
				g_selection = TILE_COUNT - 1;
			}
			UpdateSelection(g_selection);
		} else if(g_keys['L']) {
			if(++g_selection > TILE_COUNT - 1) {
				g_selection = 1;
			}
			UpdateSelection(g_selection);
		}
		if(g_game && g_keys['P']) {
			PauseGame();
			g_keys['P'] = FALSE;
		}
		return 0;
	case WM_SYSKEYUP:
		if(wParam == VK_MENU) {
			return DefWindowProc(hWnd, uMsg, wParam, lParam);
		} /*Falls through*/
	case WM_KEYUP:
		g_keys[wParam] = FALSE;
		return 0;
	case WM_COMMAND:
		switch(LOWORD(wParam)) {
		case ID_NEW:
			if(WndContinue()) {
				g_path[0] = '\0';
				g_change = FALSE;
				InitialzeRoomDefault();
				UpdateAndInvalidate();
			}
			break;
		case ID_OPEN:
			if(WndContinue()) {
				ofn = (OPENFILENAME) {
					.lStructSize = sizeof(ofn),
					.hwndOwner = hWnd,
					.lpstrFilter = "Quasar Map (*.qua)\0*.qua\0",
					.lpstrFile = path,
					.nMaxFile = MAX_PATH,
					.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
					.lpstrDefExt = "qua",
				};
				if(GetOpenFileName(&ofn)) {
					if(!MapLoad(path)) {
						MessageBox(hWnd, "Map could not be loaded", "Error", MB_ICONERROR);
					}
					UpdateAndInvalidate();
				}
			}
			break;
		case ID_SAVE:
			if(g_path[0]) {
				MapSave(g_path);	
				break;
			} /*Falls through*/
		case ID_SAVEAS:
			path[0] = '\0';
			ofn = (OPENFILENAME) {
				.lStructSize = sizeof(ofn),
				.hwndOwner = hWnd,
				.lpstrFilter = "Quasar Map (*.qua)\0*.qua\0",
				.lpstrFile = path,
				.nMaxFile = MAX_PATH,
				.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
				.lpstrDefExt = "qua"
			};
			if(GetSaveFileName(&ofn) && MapSave(path)) {
				strcpy(g_path, path);
			}
			break;
		case ID_RESIZE:
			DialogBox(NULL, "ResizeMapDialog", hWnd, DlgProc);
			break;
		case ID_INSERT:
			if(g_place.exists) {
				CheckMenuItem(g_menu, ID_INSERT, MF_UNCHECKED);
			} else {
				CheckMenuItem(g_menu, ID_INSERT, MF_CHECKED);
			}
			g_place.exists ^= TRUE;
			UpdateAndInvalidate();
			break;
		case ID_HOME:
			if(g_player.pos.x >= 0.0f && g_player.pos.x < g_room.width << 4 &&
					g_player.pos.y >= 0.0f && g_player.pos.y < g_room.height << 4) {
				g_place.pos.x = ALIGN_TO_TILE(g_player.pos.x);
				g_place.pos.y = ALIGN_TO_TILE(g_player.pos.y);
				if(!g_game) {
					g_camera.pos.x = GetPosFromCenterFocus(g_player.pos.x);
					g_camera.pos.y = 0.0f;
				}
				UpdateAndInvalidate();
			}
			break;
		case ID_LOCK:
			CheckMenuItem(g_menu, ID_LOCK, g_locked ? MF_UNCHECKED : MF_CHECKED);
			g_locked ^= TRUE;
			break;	
		case ID_PLAYER:
			CheckMenuItem(g_menu, ID_PLAYER, 
			              g_player.state & PLAYER_STATE_SMALL ? MF_UNCHECKED : MF_CHECKED);
			g_player.state ^= PLAYER_STATE_SMALL;
			break;
		case ID_RUN:
			g_game ^= TRUE;
			if(g_game) {
				/*GameSetUp*/
				g_camera.pos.x = 0;
				g_camera.pos.y = 0;
				CheckMenuItem(g_menu, ID_RUN, MF_CHECKED);
				EnableMenuItem(g_menu, ID_PAUSED,  MF_ENABLED);
				EnableMenuItem(g_menu, ID_PLAYER, MF_ENABLED);
				PlayWave(g_waves[WAVE_MUSIC]);
			} else {
				/*GameCleanUp*/
				StopWave(g_waves[WAVE_MUSIC]);
				CheckMenuItem(g_menu, ID_RUN, MF_UNCHECKED);
				CheckMenuItem(g_menu, ID_PAUSED, MF_UNCHECKED);
				EnableMenuItem(g_menu, ID_PAUSED, MF_GRAYED);
				CheckMenuItem(g_menu, ID_PLAYER, MF_UNCHECKED);
				EnableMenuItem(g_menu, ID_PLAYER, MF_GRAYED);
				g_paused = FALSE;
				ResetRoom();
				UpdateAndInvalidate();
			}
			break;
		case ID_SOUND:
			if(volume == DSBVOLUME_MAX) {
				if(SUCCEEDED(IDirectSoundBuffer_SetVolume(g_primary, DSBVOLUME_MIN))) {
					volume = DSBVOLUME_MIN;
					CheckMenuItem(g_menu, ID_SOUND, MF_UNCHECKED);
				}	
			} else if(SUCCEEDED(IDirectSoundBuffer_SetVolume(g_primary, DSBVOLUME_MAX))) {
				volume = DSBVOLUME_MAX;
				CheckMenuItem(g_menu, ID_SOUND, MF_CHECKED);
			}	
			break;
		case ID_PAUSED: 
			PauseGame();
			break;
		case ID_SLOW:
			if(g_fps == 60) {
				g_fps = 6;
				CheckMenuItem(g_menu, ID_SLOW, MF_CHECKED);
			} else {
				g_fps = 60;
				CheckMenuItem(g_menu, ID_SLOW, MF_UNCHECKED);
			}
			for(int i = 0; i < WAVE_COUNT; i++) {
				IDirectSoundBuffer *wave = g_waves[i];
				if(wave) {
					DWORD status = 0;
					IDirectSoundBuffer_GetStatus(wave, &status);
					StopWave(wave);
					int sampleRate = g_format.nSamplesPerSec * g_fps / 60;
					IDirectSoundBuffer_SetFrequency(wave, sampleRate); 
					if(status & DSBSTATUS_PLAYING) {
						IDirectSoundBuffer_Play(g_waves[i], 0, 0, 0);
					}
				}
			}
			break;
		}
		return 0;
	case WM_INITMENU:
		ZeroMemory(g_keys, sizeof(g_keys));
		return 0;
    	case WM_MOUSEMOVE:
		xPos = GET_X_LPARAM(lParam);
		yPos = GET_Y_LPARAM(lParam);
		if(wParam & MK_SHIFT && xPos < BUFFER_WIDTH * g_ctrr && yPos < BUFFER_HEIGHT * g_ctrr) {
			if(wParam & MK_LBUTTON) {
				PlaceTile(xPos / g_ctrr + g_camera.pos.x, yPos / g_ctrr + g_camera.pos.y, 
						g_selection, FALSE);
			} else if(wParam & MK_RBUTTON) {
				PlaceTile(xPos / g_ctrr + g_camera.pos.x, yPos / g_ctrr + g_camera.pos.y, 
						0, FALSE);
			}
		}
		return 0;
	case WM_LBUTTONDOWN:
		xPos = GET_X_LPARAM(lParam);
		yPos = GET_Y_LPARAM(lParam);
		if(xPos < BUFFER_WIDTH * g_ctrr) {
			if(yPos < BUFFER_HEIGHT * g_ctrr) {
				PlaceTile(xPos / g_ctrr + g_camera.pos.x, yPos / g_ctrr + g_camera.pos.y, 
						g_selection, FALSE);
			}
		} else if(xPos < BUFFER_WIDTH * g_ctrr + SELECT_WIDTH * g_ctrr && 
				yPos < SELECT_HEIGHT * g_ctrr) {
			xPos = ((xPos - BUFFER_WIDTH * g_ctrr) / g_ctrr >> 4) + 1;
			UpdateSelection(xPos);	
		}
		return 0;
	case WM_RBUTTONDOWN:
		xPos = GET_X_LPARAM(lParam);
		yPos = GET_Y_LPARAM(lParam);
		if(xPos < BUFFER_WIDTH * g_ctrr && yPos < BUFFER_HEIGHT * g_ctrr) {
			PlaceTile(xPos / g_ctrr + g_camera.pos.x, yPos / g_ctrr + g_camera.pos.y, 0, FALSE);
		}
		return 0;
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}
static BOOL Collision(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2) {
	return x1 + w1 >= x2 && x1 <= x2 + w2 && y1 + h1 >= y2 && y1 <= y2 + h2;
}

static void RemoveEntity(int i) {
	g_entitiesUsed--;
	for(int j = i; j < g_entitiesUsed; j++) {
		g_entities[i] = g_entities[j + 1];	
	}
}

static struct BitmapAlpha CreateBitmapAlpha(const char *path, COLORREF crMask) {
	struct BitmapAlpha ba;
	/*Create bitmap*/
	ba.color = LoadImage(NULL, path, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
	BITMAP bm;
	GetObject(ba.color, sizeof(bm), &bm);
	ba.mask = CreateBitmap(bm.bmWidth, bm.bmHeight, 1, 1, NULL);
	/*Create mask*/
	HDC hdcColor = CreateCompatibleDC(NULL);
	HDC hdcMask = CreateCompatibleDC(NULL);
	HBITMAP hbmOldColor = SelectObject(hdcColor, ba.color);
	HBITMAP hbmOldMask = SelectObject(hdcMask, ba.mask);

	SetBkColor(hdcColor, crMask); 
	BitBlt(hdcMask, 0, 0, bm.bmWidth, bm.bmHeight, hdcColor, 0, 0, SRCCOPY);
	BitBlt(hdcColor, 0, 0, bm.bmWidth, bm.bmHeight, hdcMask, 0, 0, SRCINVERT);

	SelectObject(hdcMask, hbmOldMask);
	SelectObject(hdcColor, hbmOldColor);
	DeleteDC(hdcMask);
	DeleteDC(hdcColor);
	return ba;
}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hInstancePrev, LPSTR lpCmdLine, int nShowCmd) {
	/*SetUpBuffers*/
	HDC hdcScreen = GetDC(NULL);
	g_hbmSelect = CreateCompatibleBitmap(hdcScreen, SELECT_WIDTH, SELECT_HEIGHT); 
	g_hbmBuffer = CreateCompatibleBitmap(hdcScreen, BUFFER_WIDTH, BUFFER_HEIGHT);
	ReleaseDC(NULL, hdcScreen);
	/*CreateScratchBitmap*/
	COLORREF crBlackWithAlpha = 0xFF000000;
	g_hbmScratch = CreateBitmap(1, 1, 1, 32, &crBlackWithAlpha); 
	/*CreateAlphaBitmap*/
	g_baMario = CreateBitmapAlpha("images/mario.bmp", RGB(0, 136, 255));
	g_baTiles = CreateBitmapAlpha("images/tiles.bmp", RGB(0, 64, 128));
	g_baBackgrounds = CreateBitmapAlpha("images/backgrounds.bmp", RGB(248, 224, 176));
	/*RoomCreate*/
	InitialzeRoomDefault();
	RenderBuffer();
	RenderSelector();
	/*SpawnDinosaur*/
	struct Entity *entity = &g_entities[g_entitiesUsed];
	entity->type = ENTITY_DINOSAUR;
	entity->tick = 0;
	entity->pos.x = 32;
	entity->pos.y = g_player.pos.y - 2.0f;
	entity->vel.x = 0.5f;
	entity->vel.y = 0.0f;
	g_entitiesUsed++;	
	/*CreateQuasarWindow*/
	WNDCLASS wc;
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = hInstance;
	wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = NULL;
	wc.lpszMenuName = "LevelEditorMenu";
	wc.lpszClassName = "Quasar";
	RegisterClass(&wc);
	/*Create fullscreen window on primary monitor*/
	POINT tl = {0, 0}; 
	MONITORINFO monInfo = {sizeof(monInfo)};
	GetMonitorInfo(MonitorFromPoint(tl, MONITOR_DEFAULTTOPRIMARY), &monInfo);
	int cxMon = monInfo.rcMonitor.right - monInfo.rcMonitor.left;
	int cyMon = monInfo.rcMonitor.bottom - monInfo.rcMonitor.top;
	g_ctrr = min(cxMon / (BUFFER_WIDTH + SELECT_WIDTH), cyMon / BUFFER_HEIGHT);
	g_hWnd = CreateWindow("Quasar", "Super Mario World", WS_POPUP | WS_VISIBLE, 
			0, 0, cxMon, cyMon, NULL, NULL, hInstance, NULL);
	if(!g_hWnd) {
		MessageBox(NULL, "Could not create window!", "Error", MB_ICONERROR);
		return 2;
	}
	g_menu = GetMenu(g_hWnd);
	/*Timing*/
	LARGE_INTEGER perf;
	QueryPerformanceFrequency(&perf);
	/*Set up audio*/
	DSBUFFERDESC primaryDesc = {
		.dwSize = sizeof(DSBUFFERDESC),
		.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME
	};
	HRESULT WINAPI(*dyDirectSoundCreate)(LPCGUID, struct IDirectSound **, LPUNKNOWN) = 
			(void *) GetProcAddress(LoadLibrary("dsound.dll"), "DirectSoundCreate");
	if(dyDirectSoundCreate && SUCCEEDED(dyDirectSoundCreate(NULL, &g_dsound, NULL)) && 
			SUCCEEDED(IDirectSound_SetCooperativeLevel(g_dsound, g_hWnd, DSSCL_PRIORITY)) &&
			SUCCEEDED(IDirectSound_CreateSoundBuffer(g_dsound, &primaryDesc, &g_primary, NULL)) &&
			SUCCEEDED(IDirectSoundBuffer_SetVolume(g_primary, DSBVOLUME_MIN)) &&
			SUCCEEDED(IDirectSoundBuffer_SetFormat(g_primary, &g_format))) {
		g_waves[WAVE_MUSIC] = LoadWave("music/overworld.wav");
		g_waves[WAVE_JUMP] = LoadWave("sounds/jump.wav");
		g_waves[WAVE_LOST] = LoadWave("sounds/lost.wav");
		g_waves[WAVE_APPEARS] = LoadWave("sounds/appears.wav");
		g_waves[WAVE_POWER] = LoadWave("sounds/power.wav");
		g_waves[WAVE_COIN] = LoadWave("sounds/coin.wav");
		g_waves[WAVE_RICOCHET] = LoadWave("sounds/ricochet.wav");
	} else {
		g_dsound = NULL;	
		EnableMenuItem(g_menu, ID_SOUND, MF_GRAYED);
	}

	/*Load X-Input*/
	HMODULE xInputLib = LoadLibrary("xinput1_4.dll") ? : 
			LoadLibrary("xinput1_3.dll") ? :
			LoadLibrary("xinput9_1_0.dll");
	if(xInputLib) {
		dyXInputGetState = (void *) GetProcAddress(xInputLib, "XInputGetState");
		if(!dyXInputGetState) {
			FreeLibrary(xInputLib);
		}
	}	

	/*Message loop*/
	HACCEL acc = LoadAccelerators(hInstance, "LevelEditorAcc");
	MSG msg;
	while(GetMessage(&msg, NULL, 0, 0)) {
		if(!TranslateAccelerator(g_hWnd, acc, &msg)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		if(g_game && !g_paused) {
			BOOL granular = timeBeginPeriod(1) == TIMERR_NOERROR;
			LARGE_INTEGER begin, end;
			XINPUT_GAMEPAD prev = {};
			QueryPerformanceCounter(&begin);
			do {
				while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
					if(msg.message == WM_QUIT) {
						timeEndPeriod(1);
						return msg.wParam;
					}
					if(!TranslateAccelerator(g_hWnd, acc, &msg)) {
						TranslateMessage(&msg);
						DispatchMessage(&msg);
					}
				}
				/*Game logic*/
				if(g_player.state & PLAYER_STATE_DEAD) {
					if(g_player.tick-- <= 0) {
						ResetRoom();	
						PlayWave(g_waves[WAVE_MUSIC]);
					}
				} else if(g_player.state & PLAYER_STATE_TRANSITION) {
					if(g_player.tick-- > 0) {
						if(g_player.tick % 8 < 4) {
							/*HEAD STATE*/
							g_player.sprite.x = IsPlayerFacingLeft() ? 6 : 366;
							g_player.sprite.y = 786;
						} else if(g_player.tick < 20) {
							/*BIG STATE*/	
							g_player.sprite.x = IsPlayerFacingLeft() ? 166 : 206;
							g_player.sprite.y = 74;
						} else {
							/*SMALL STATE*/	
							g_player.sprite.x = IsPlayerFacingLeft() ? 166 : 206;
							g_player.sprite.y = -10;
						}
					} else {
						g_player.state &= ~(PLAYER_STATE_TRANSITION | PLAYER_STATE_SMALL);
					}
				} else {
					/*HandleEntities*/
					for(int i = 0; i < g_entitiesUsed; ) {
						struct Entity *entity = &g_entities[i];
						switch(entity->type) {
						case ENTITY_MUSHROOM:
							if(fabsf(entity->pos.x - g_camera.pos.x) > BUFFER_WIDTH) {
								RemoveEntity(i);
							} else {
								entity->pos.x += entity->vel.x;
								entity->pos.y += entity->vel.y;
								if(entity->tick <= 0) {
									int yCom = ((int) entity->pos.y >> 4) + 1;
									struct TilePair pair = TilePairBelow(entity->pos, entity->vel, 3, 14, 1);
									if((g_tiles[*pair.left].prop | 
												g_tiles[*pair.right].prop) & TILE_PROP_TOP_SOLID) {
										entity->vel.y = 0.0f;
										entity->pos.y = (int) entity->pos.y & ~15;
									} else {
										entity->vel.y += 0.25f;
									}
									if((int) entity->pos.x >> 4 >= g_room.width - 1 || 
											MushroomSolidHorzTileOccurred(entity->pos.x, 
													entity->pos.y, 14, TILE_PROP_LEFT_SOLID)) {
										entity->vel.x = -1.0f;
										/*Error case for when mushroom is stuck*/
										if(entity->pos.x < 0 || MushroomSolidHorzTileOccurred(entity->pos.x, 
												entity->pos.y, 3, TILE_PROP_RIGHT_SOLID)) {
										}
									} else if(entity->pos.x < 0 || MushroomSolidHorzTileOccurred(entity->pos.x, 
												entity->pos.y, 3, TILE_PROP_RIGHT_SOLID)) {
										entity->vel.x = 1.0f;
									}
								} else if(--entity->tick == 0)  {
									entity->vel.x = 1.0f;
									entity->vel.y = 0.0f;
								}
								if(entity->pos.y >= g_room.height << 4) {
									RemoveEntity(i);
								} else {
									int yDsp = g_player.state & PLAYER_STATE_SMALL ? 10 : 0;
									if(Collision(g_player.pos.x + 5, g_player.pos.y + 3 + yDsp, 10, 24 - yDsp, 
											entity->pos.x, entity->pos.y, 16, 14)) {
										if(g_player.state & PLAYER_STATE_SMALL) {
											g_player.state |= PLAYER_STATE_TRANSITION;	
										}
										RemoveEntity(i);
										PlayWave(g_waves[WAVE_POWER]);
									} else {
										i++;
									}
								}
							}
							break;
						case ENTITY_DINOSAUR:
							entity->pos.x += entity->vel.x;
							entity->pos.y += entity->vel.y;
							entity->tick++;
							i++;
							break;
						}
					}
					/*HandleAscend*/
					for(int i = 0; i < g_ascendsUsed; ) {
						if(g_ascends[i].yVel < g_ascends[i].yVelMax) {
							g_ascends[i].pos.y += g_ascends[i].yVel;
							g_ascends[i].yVel += g_ascends[i].yAcc;
							if(g_ascends[i].yVel == g_ascends[i].yVelMax) {
								struct FPoint pos = g_ascends[i].pos;
								BYTE *tile = &g_room.tiles[((int) pos.y >> 4) + 1][(int) pos.x >> 4];
								switch(g_ascends[i].sprite.x) {
								case 17:
									*tile = TILE_USED_ITEM_BLOCK;	
									break;
								case 255:
									*tile = TILE_USED_QUESTION;
									break;
								case 290:
									if(g_effectsUsed < _countof(g_effects)) {
										g_effects[g_effectsUsed].pos.x = pos.x;
										g_effects[g_effectsUsed].pos.y = pos.y;
										g_effects[g_effectsUsed].tick = 35;
										g_effectsUsed++;
									}
									break;
								}
							}
							i++;
						} else {
							if(g_ascends[i].sprite.x == 17 && g_entitiesUsed < _countof(g_entities)) {
								struct Entity *entity = &g_entities[g_entitiesUsed];
								entity->type = ENTITY_MUSHROOM;
								entity->tick = 52;
								entity->pos.x = g_ascends[i].pos.x;
								entity->pos.y = g_ascends[i].pos.y + 3.0f;
								entity->vel.x = 0.0f;
								entity->vel.y = -0.25f;
								g_entitiesUsed++;	
								PlayWave(g_waves[WAVE_APPEARS]);
							}
							g_ascends[i] = g_ascends[--g_ascendsUsed];	
						}
					}
					/*UpdateEffects*/
					for(int i = 0; i < g_effectsUsed; ) {
						if(g_effects[i].tick > 0) {
							g_effects[i].tick--;	
							i++;
						} else {
							g_effects[i] = g_effects[--g_effectsUsed];	
						}
					}
					/*UpdatePlayer*/
					XINPUT_STATE cur = {};
					if(dyXInputGetState) {
						dyXInputGetState(0, &cur);
					}
					/*Horiztontal Motion*/
					if(g_keys[VK_LEFT] || cur.Gamepad.sThumbLX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
						if(g_keys[VK_RIGHT]) {
							SetPlayerStateToIdle();
						} else {
							/*Speed cases*/
							if(g_player.vel.x > -WALKING_SPEED) {
								g_player.vel.x -= 0.25f;
							} else if(g_keys[VK_SHIFT] || cur.Gamepad.wButtons & XINPUT_GAMEPAD_Y) {
								if(g_player.vel.x > -RUNNING_SPEED) {
									g_player.vel.x -= 0.0625f;
								} else if(g_player.vel.x > -SPRINTING_SPEED) {
									g_player.vel.x -= 0.015625f;
								}
							} else if(g_player.vel.x < -WALKING_SPEED) {
								if(g_player.vel.x > -1.25f) {
									g_player.vel.x = -WALKING_SPEED;
								} else {
									g_player.vel.x += 0.015625f;
								}
							}
							/*Animation cases*/
							if(g_player.state & PLAYER_STATE_JUMPING) {
								SetPlayerSpriteToJumpLeft();
							} else if(g_player.state & PLAYER_STATE_FALLING) {
								SetPlayerSpriteToFallingLeft();
							} else {
								AnimatePlayerWalkingLeft();
							}
						}
					} else if(g_keys[VK_RIGHT] || cur.Gamepad.sThumbLX > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
						if(g_player.vel.x < WALKING_SPEED) {
							g_player.vel.x += 0.25f;
						} else if(g_keys[VK_SHIFT] || cur.Gamepad.wButtons & XINPUT_GAMEPAD_Y) {
							if(g_player.vel.x < RUNNING_SPEED) {
								g_player.vel.x += 0.0625f;
							} else if(g_player.vel.x < SPRINTING_SPEED) {
								g_player.vel.x += 0.015625f;
							}
						} else if(g_player.vel.x > WALKING_SPEED) {
							if(g_player.vel.x < 1.25f) {
								g_player.vel.x = WALKING_SPEED;
							} else {
								g_player.vel.x -= 0.25f;
							}
						}
						if(g_player.state & PLAYER_STATE_JUMPING) {
							SetPlayerSpriteToJumpRight();
						} else if(g_player.state & PLAYER_STATE_FALLING) {
							SetPlayerSpriteToFallingRight();
						} else {
							AnimatePlayerWalkingRight();
						}
					} else {
						SetPlayerStateToIdle();
					}
					/*Move player and bound player horizontally*/
					g_player.pos.x += g_player.vel.x;
					if(g_player.pos.x < 0.0f) {
						g_player.pos.x = 0.0f;
						g_player.vel.x = 0.0f;
					} else if(g_player.pos.x > (g_room.width << 4) - 20.0f) {
						g_player.pos.x = (g_room.width << 4) - 20.0f;
						g_player.vel.x = 0.0f;
					}
					/*HandlePlayerHorzCollisons*/
					struct TileTriple triple = TileTripleHorz(g_player.pos, g_player.vel);
					if(*triple.head == TILE_COIN) {
						CollectCoin(triple.head);
					}
					if(*triple.body == TILE_COIN) {
						CollectCoin(triple.body);
					}
					if(*triple.feet == TILE_COIN) {
						CollectCoin(triple.feet);
					}
					/*Vertical Motion*/
					g_keys[VK_SPACE] |= cur.Gamepad.wButtons & XINPUT_GAMEPAD_B && !(prev.wButtons & XINPUT_GAMEPAD_B); 
					if(g_player.state & PLAYER_STATE_JUMPING) {
						if(g_player.vel.y > 0.5f) {
							g_keys[VK_SPACE] = FALSE;
							SetPlayerStateToFalling();
							g_player.vel.y += 0.25f;
						} else {
							if(!g_keys[VK_SPACE] && g_player.tick < 1) {
								g_player.tick = 4;
							}
							if(++g_player.tick > 3) {
								g_player.vel.y += 0.25f;
							}
						}
					} else {
						struct TilePair pair = TilePairBelow(g_player.pos, g_player.vel, 5, 12, 2); 
						if((g_tiles[*pair.left].prop | g_tiles[*pair.right].prop) & TILE_PROP_TOP_SOLID) { 
							/*Player jumps or play stops falling*/
							if(g_keys[VK_SPACE] && !(g_player.state & (PLAYER_STATE_JUMPING | PLAYER_STATE_FALLING))) {
								PlayWave(g_waves[WAVE_JUMP]);
								if(fabsf(g_player.vel.x) < RUNNING_SPEED) {
									g_player.vel.y = -5.25f;
								} else if(fabsf(g_player.vel.x) < SPRINTING_SPEED) { 
									g_player.vel.y = -5.75f;
								} else {
									g_player.vel.y = -6.25f;
								}
								g_player.state &= ~PLAYER_STATE_FALLING;
								g_player.state |= PLAYER_STATE_JUMPING;
								if(IsPlayerFacingLeft()) {
									SetPlayerSpriteToJumpLeft();
								} else {
									SetPlayerSpriteToJumpRight();
								}
								g_player.tick = 0;
							} else {
								if(g_player.state & PLAYER_STATE_FALLING) {
									g_player.state &= ~(PLAYER_STATE_FALLING | PLAYER_STATE_JUMPING);
									SetPlayerToIdleAnimation();
								}
								g_player.pos.y = ALIGN_TO_TILE(g_player.pos.y) + 3.0f;
								g_player.vel.y = 0.0f;
							}
						} else {
							/*Update Falling Animation*/
							if(g_player.vel.y < 6.0f) {
								g_player.vel.y += 0.25f;
							}
							SetPlayerStateToFalling();
						}
					}
					/*Move player and bound player vertically*/
					g_player.pos.y += g_player.vel.y;
					int yCom = g_player.state & PLAYER_STATE_SMALL ?
							(int) (g_player.pos.y + 18) >> 4:
							(int) (g_player.pos.y + 8) >> 4;
					if(yCom >= 0 && yCom < g_room.height) {
						/*TilePairAbove*/
						int xComLeft = ((int) g_player.pos.x + 5) >> 4;
						int xComRight = ((int) g_player.pos.x + 12) >> 4;
						struct TilePair pair;
						pair.left = &g_room.tiles[yCom][xComLeft];
						pair.right = &g_room.tiles[yCom][xComRight];	
						if((g_tiles[*pair.left].prop | g_tiles[*pair.right].prop) & TILE_PROP_BOTTOM_SOLID) {
							g_player.pos.y = ALIGN_TO_TILE(g_player.pos.y);
							g_player.pos.y += g_player.state & PLAYER_STATE_SMALL ? 15.0f : 9.0f;
							g_player.vel.y = 0.0f;
							g_player.state &= ~PLAYER_STATE_JUMPING;
							g_player.state |= PLAYER_STATE_FALLING;
							g_keys[VK_SPACE] = FALSE;
							PlayWave(g_waves[WAVE_RICOCHET]);
						}	
						/*HandleAscendAbove*/
						if(g_ascendsUsed < _countof(g_ascends)) {
							struct FPoint pos;
							BYTE *tile;
							if((int) g_player.pos.x % 8) { 
								pos.x = xComRight << 4;
								tile = pair.right;
							} else {
								pos.x = xComLeft << 4;
								tile = pair.left;
							}
							pos.y = (yCom << 4) + 1.0f;
							struct Ascend *ascend = &g_ascends[g_ascendsUsed];
							switch(*tile) {
							case TILE_QUESTION:
								ascend->pos = pos;
								ascend->sprite.x = 255;
								ascend->sprite.y = 289;
								ascend->yVel = -4.0f;
								ascend->yVelMax = 3.0f;
								ascend->yAcc = 1.0f;
								g_ascendsUsed++;
								*tile = TILE_INVISIBLE_SOLID;
								if(g_ascendsUsed < _countof(g_ascends)) {
									ascend = &g_ascends[g_ascendsUsed];
									ascend->pos.x = pos.x;
									ascend->pos.y = pos.y - 19;
									ascend->sprite = g_tiles[TILE_COIN].sprite;
									ascend->yVel = -3.0f;
									ascend->yVelMax = 2.0f;
									ascend->yAcc = 0.25f;
									g_ascendsUsed++;
									PlayWave(g_waves[WAVE_COIN]);
								}
								break;
							case TILE_ITEM_BLOCK:
								ascend->pos = pos;
								ascend->sprite.x = 17;
								ascend->sprite.y = 0;
								ascend->yVel = -4.0f;
								ascend->yVelMax = 3.0f;
								ascend->yAcc = 1.0f;
								g_ascendsUsed++;
								*tile = TILE_INVISIBLE_SOLID;
								break;
							}
						}
						/*CollectCoinPair*/
						if(*pair.left == TILE_COIN) {
							CollectCoin(pair.left);
						} 
						if(*pair.right == TILE_COIN) {
							CollectCoin(pair.right);
						}
					} else if(g_player.pos.y > (g_room.height + 2) << 4) {
						g_player.state |= PLAYER_STATE_DEAD;
						StopWave(g_waves[WAVE_MUSIC]);
						PlayWave(g_waves[WAVE_LOST]);
						g_player.tick = 240;
					}
					/*Set input to previous input*/
					prev = cur.Gamepad;
					if(g_player.state & PLAYER_STATE_TRANSITION) {
						g_player.tick = 40;
					}
					/*UpdateCamera*/
					if(!g_locked) {
						float xMove = g_player.vel.x;
						int cxWidth = g_room.width << 4;
						if(g_player.pos.x > 136.0f && xMove > 0.0f) {
							if(g_camera.focus < 0.0f) {
								g_camera.focus += xMove;
							} else {
								g_camera.focus = 16.0f;
								if(g_camera.offset < 48.0f) {
									xMove += 2.0f;
									g_camera.offset += xMove;
								}
								g_camera.pos.x += xMove;
							}
						} else if(g_player.pos.x < cxWidth - 136.0f && xMove < 0.0f) {
							if(g_camera.focus > 0.0f) {
								g_camera.focus += xMove;
							} else {
								g_camera.focus = -16.0f;
								if(g_camera.offset > 0.0f) {
									xMove -= 2.0f;
									g_camera.offset += xMove;
								}
								g_camera.pos.x += xMove;
							}
						}
						/*Stops cam from moving horizontally offscreen*/
						if(g_camera.pos.x < 0.0f) {
							g_camera.focus = 0.0f;
							g_camera.offset = 0.0f;
							g_camera.pos.x = 0.0f;
						} else if(g_camera.pos.x > cxWidth - BUFFER_WIDTH) {
							g_camera.focus = 0.0f;
							g_camera.offset = 0.0f;
							g_camera.pos.x = cxWidth - BUFFER_WIDTH;
						}
					}
				}
				RenderBuffer();
				/*Rest till next frame*/
				QueryPerformanceCounter(&end);
				LONGLONG remain = perf.QuadPart / g_fps - end.QuadPart + begin.QuadPart;
				if(remain > 0) {
					if(granular) {
						DWORD sleep = 1000 * remain / perf.QuadPart;
						if(sleep) {
							Sleep(sleep);
						}
					}
					do {
						QueryPerformanceCounter(&end);
						remain = perf.QuadPart / g_fps - end.QuadPart + begin.QuadPart;
					} while(remain > 0);
				}
				QueryPerformanceCounter(&begin);
				/*Draw window and possibly tick next frame*/
				HDC hdcWindow = GetDC(g_hWnd);
				RenderToScreen(hdcWindow);
				ReleaseDC(g_hWnd, hdcWindow);	
				if(!(g_player.state & (PLAYER_STATE_DEAD | PLAYER_STATE_TRANSITION))) {
					g_tick++;
				}
			} while(g_game && !g_paused);
			timeEndPeriod(1);
		}
	}
	return msg.wParam;
}
