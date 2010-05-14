#include "stdafx.h"
#include "SuperCalc.h"

#pragma warning(disable: 4127)

#define _WIN32_WINNT 0x0501
#include <windows.h>

#include "commctrl.h"
#pragma comment(lib, "comctl32.lib")
#include <windowsx.h>

#pragma warning(disable: 4201)
#include <MMSystem.h>
#pragma comment(lib, "Winmm.lib")
#pragma warning(default: 4201)

#pragma comment(lib, "Ws2_32.lib")

#include <iostream>

#include "NULLC/nullc.h"
#include "NULLC/nullc_debug.h"

#include "Colorer.h"

#include "UnitTests.h"

#include "GUI/RichTextarea.h"
#include "GUI/TabbedFiles.h"

// NULLC modules
#include "NULLC/includes/file.h"
#include "NULLC/includes/io.h"
#include "NULLC/includes/math.h"
#include "NULLC/includes/string.h"
#include "NULLC/includes/vector.h"
#include "NULLC/includes/list.h"
#include "NULLC/includes/map.h"
#include "NULLC/includes/hashmap.h"
#include "NULLC/includes/random.h"
#include "NULLC/includes/time.h"
#include "NULLC/includes/gc.h"

#include "NULLC/includes/window.h"

#include "NULLC/includes/canvas.h"

#define MAX_LOADSTRING 100

HINSTANCE hInst;
char szTitle[MAX_LOADSTRING];
char szWindowClass[MAX_LOADSTRING];

WORD				MyRegisterClass(HINSTANCE hInstance);
bool				InitInstance(HINSTANCE, int);
LRESULT CALLBACK	WndProc(HWND, unsigned int, WPARAM, LPARAM);
LRESULT CALLBACK	About(HWND, unsigned int, WPARAM, LPARAM);

// Window handles
HWND hWnd;			// Main window
HWND hButtonCalc;	// Run/Abort button
HWND hContinue;		// Button that continues an interrupted execution
HWND hJITEnabled;	// JiT enable check box
HWND hTabs;	
HWND hNewTab, hNewFilename, hNewFile;
HWND hResult;		// label with execution result
HWND hCode;			// disabled text area for error messages and other information
HWND hVars;			// disabled text area that shows values of all variables in global scope
HWND hStatus;		// Main window status bar

HWND hAttachPanel, hAttachList, hAttachDo, hAttachBack, hAttachTabs;
bool stateAttach = false, stateRemote = false;
CRITICAL_SECTION	pipeSection;

unsigned int areaWidth = 400, areaHeight = 300;

HFONT	fontMonospace, fontDefault;

Colorer*	colorer;

struct Breakpoint
{
	HWND	tab;
	unsigned int	line;
	bool	valid;
};

std::vector<HWND>	richEdits;
std::vector<HWND>	attachedEdits;

const unsigned int INIT_BUFFER_SIZE = 4096;
char	initError[INIT_BUFFER_SIZE];

// for text update
bool needTextUpdate;
DWORD lastUpdate;

//////////////////////////////////////////////////////////////////////////
// Remote debugging
enum DebugCommand
{
	DEBUG_REPORT_INFO,
	DEBUG_MODULE_INFO,
	DEBUG_MODULE_NAMES,
	DEBUG_SOURCE_INFO,
	DEBUG_TYPE_INFO,
	DEBUG_VARIABLE_INFO,
	DEBUG_FUNCTION_INFO,
	DEBUG_LOCAL_INFO,
	DEBUG_TYPE_EXTRA_INFO,
	DEBUG_SYMBOL_INFO,
	DEBUG_CODE_INFO,
	DEBUG_BREAK_SET,
	DEBUG_BREAK_HIT,
	DEBUG_BREAK_CONTINUE,
	DEBUG_BREAK_STACK,
	DEBUG_BREAK_CALLSTACK,
	DEBUG_BREAK_DATA,
	DEBUG_DETACH,
};

struct PipeData
{
	DebugCommand	cmd;
	bool			question;

	union
	{
		struct Report
		{
			unsigned int	pID;
			char			module[256];
		} report;
		struct Data
		{
			unsigned int	wholeSize;
			unsigned int	dataSize;
			unsigned int	elemCount;
			char			data[512];
		} data;
		struct Debug
		{
			unsigned int	breakInst;
			bool			breakSet;
		} debug;
	};
};
HANDLE	pipeThread = INVALID_HANDLE_VALUE;

namespace RemoteData
{
	ExternModuleInfo	*modules = NULL;
	unsigned int		moduleCount = 0;
	char				*moduleNames = NULL;

	unsigned int		infoSize = 0;
	NULLCCodeInfo		*codeInfo = NULL;

	char				*sourceCode = NULL;

	unsigned int		varCount = 0;
	ExternVarInfo		*vars = NULL;

	unsigned int		typeCount = 0;
	ExternTypeInfo		*types = NULL;

	unsigned int		funcCount = 0;
	ExternFuncInfo		*functions = NULL;

	unsigned int		localCount = 0;
	ExternLocalInfo		*locals = NULL;

	unsigned int		*typeExtra = NULL;
	char				*symbols = NULL;

	char				*stackData = NULL;
	unsigned int		stackSize = 0;

	unsigned int		callStackFrames = 0;
	unsigned int		*callStack = NULL;

	sockaddr_in		saServer;
	hostent			*localHost;
	char			*localIP;
	SOCKET			sck;

	int SocketSend(SOCKET sck, char* source, size_t size, int timeOut)
	{
		TIMEVAL tv = { timeOut, 0 };
		fd_set	fdSet;
		FD_ZERO(&fdSet);
		FD_SET(sck, &fdSet);

		int active = select(0, NULL, &fdSet, NULL, &tv);
		if(active == SOCKET_ERROR)
		{
			printf("select failed\n");
			return -1;
		}
		if(!FD_ISSET(sck, &fdSet))
		{
			printf("!FD_ISSET\n");
			return 0;
		}
		
		int allSize = (int)size;
		while(size)
		{
			int bytesSent;
			if((bytesSent = send(sck, source + (allSize - size), (int)size, 0)) == SOCKET_ERROR || bytesSent == 0)
			{
				printf("send failed\n");
				return -1;
			}
			size -= bytesSent;
			if(size)
				printf("Partial send\n");
		}
		return allSize;
	}
	int SocketReceive(SOCKET sck, char* destination, size_t size, int timeOut)
	{
		TIMEVAL tv = { timeOut, 0 };
		fd_set	fdSet;
		FD_ZERO(&fdSet);
		FD_SET(sck, &fdSet);

		int active = select(0, &fdSet, NULL, NULL, &tv);
		if(active == SOCKET_ERROR)
			return -1;
		if(!FD_ISSET(sck, &fdSet))
			return 0;

		int allSize = (int)size;
		while(size)
		{
			int bytesRecv;
			if((bytesRecv = recv(sck, destination + (allSize - size), (int)size, 0)) == SOCKET_ERROR || bytesRecv == 0)
			{
				printf("recv failed\n");
				return -1;
			}
			size -= bytesRecv;
			if(size)
				printf("Partial recv\n");
		}
		return allSize;
	}
}

char* GetLastErrorDesc()
{
	char* msgBuf = NULL;
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		reinterpret_cast<LPSTR>(&msgBuf), 0, NULL);
	return msgBuf;
}

bool PipeSendRequest(PipeData &data)
{
	int result = RemoteData::SocketSend(RemoteData::sck, (char*)&data, sizeof(data), 5);
	if(!result || result == -1)
	{
		if(!result)
			MessageBox(hWnd, "Failed to send data through socket (select failed)", "ERROR", MB_OK);
		else
			MessageBox(hWnd, GetLastErrorDesc(), "Error", MB_OK);
		return false;
	}
	return true;
}

char* PipeReceiveResponce(PipeData &data)
{
	unsigned int recvSize = 0;
	DebugCommand cmd = data.cmd;
	char *rawData = NULL;
	do
	{
		int result = RemoteData::SocketReceive(RemoteData::sck, (char*)&data, sizeof(data), 5);
		if(!result || result == -1 || result != sizeof(data) || data.cmd != cmd || data.question)
		{
			delete[] rawData;
			if(!result)
				MessageBox(hWnd, "Failed to receive data through socket (select failed)", "ERROR", MB_OK);
			else if(result == -1)
				MessageBox(hWnd, GetLastErrorDesc(), "Error", MB_OK);
			else
				printf("%d != %d || %d != %d || %d\n", result, sizeof(data), data.cmd, cmd, data.question);
			return NULL;
		}
		if(!recvSize)
			rawData = new char[data.data.wholeSize];
		memcpy(rawData + recvSize, data.data.data, data.data.dataSize);
		recvSize += data.data.dataSize;
	}while(recvSize < data.data.wholeSize);
	return rawData;
}

//////////////////////////////////////////////////////////////////////////

enum OverlayType
{
	OVERLAY_NONE,
	OVERLAY_CALLED,
	OVERLAY_STOP,
	OVERLAY_CURRENT,
	OVERLAY_BREAKPOINT,
	OVERLAY_BREAKPOINT_INVALID,
};
enum ExtraType
{
	EXTRA_NONE,
	EXTRA_BREAKPOINT,
	EXTRA_BREAKPOINT_INVALID,
};

void FillArrayVariableInfo(const ExternTypeInfo& type, char* ptr, HTREEITEM parent);
void FillComplexVariableInfo(const ExternTypeInfo& type, char* ptr, HTREEITEM parent);
void FillVariableInfo(const ExternTypeInfo& type, char* ptr, HTREEITEM parent);

double myGetPreciseTime()
{
	LARGE_INTEGER freq, count;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&count);
	double temp = double(count.QuadPart) / double(freq.QuadPart);
	return temp*1000.0;
}

HANDLE breakResponse = NULL;

void IDEDebugBreak()
{
	SendMessage(hWnd, WM_USER + 2, 0, 0);
	WaitForSingleObject(breakResponse, INFINITE);
}
void IDEDebugBreakEx(unsigned int instruction)
{
	(void)instruction;
	SendMessage(hWnd, WM_USER + 2, 0, 0);
	WaitForSingleObject(breakResponse, INFINITE);
}

int APIENTRY WinMain(HINSTANCE	hInstance,
					HINSTANCE	hPrevInstance,
					LPTSTR		lpCmdLine,
					int			nCmdShow)
{
	(void)lpCmdLine;
	(void)hPrevInstance;

	MSG msg;

	needTextUpdate = true;
	lastUpdate = GetTickCount();

#ifdef _DEBUG
	AllocConsole();

	freopen("CONOUT$", "w", stdout);
	freopen("CONIN$", "r", stdin);
#endif

	bool runUnitTests = false;
	if(runUnitTests)
	{
		AllocConsole();

		freopen("CONOUT$", "w", stdout);
		freopen("CONIN$", "r", stdin);

		RunTests();
	}

	nullcInit("Modules\\");

	char modulePath[MAX_PATH];
	GetModuleFileName(NULL, modulePath, MAX_PATH);

	memset(initError, 0, INIT_BUFFER_SIZE);

	// in possible, load precompiled modules from nullclib.ncm
	FILE *modulePack = fopen(sizeof(void*) == sizeof(int) ? "nullclib.ncm" : "nullclib_x64.ncm", "rb");
	if(!modulePack)
	{
		strcat(initError, "WARNING: Failed to open precompiled module file");
		strcat(initError, sizeof(void*) == sizeof(int) ? "nullclib.ncm\r\n" : "nullclib_x64.ncm\r\n");
	}else{
		fseek(modulePack, 0, SEEK_END);
		unsigned int fileSize = ftell(modulePack);
		fseek(modulePack, 0, SEEK_SET);
		char *fileContent = new char[fileSize];
		fread(fileContent, 1, fileSize, modulePack);
		fclose(modulePack);

		char *filePos = fileContent;
		while((unsigned int)(filePos - fileContent) < fileSize)
		{
			char *moduleName = filePos;
			filePos += strlen(moduleName) + 1;
			char *binaryCode = filePos;
			filePos += *(unsigned int*)binaryCode;
			nullcLoadModuleByBinary(moduleName, binaryCode);
		}

		delete[] fileContent;
	}

	if(!nullcInitTypeinfoModule())
		strcat(initError, "ERROR: Failed to init std.typeinfo module\r\n");
	if(!nullcInitDynamicModule())
		strcat(initError, "ERROR: Failed to init std.dynamic module\r\n");

	if(!nullcInitFileModule())
		strcat(initError, "ERROR: Failed to init std.file module\r\n");
	if(!nullcInitIOModule())
		strcat(initError, "ERROR: Failed to init std.io module\r\n");
	if(!nullcInitMathModule())
		strcat(initError, "ERROR: Failed to init std.math module\r\n");
	if(!nullcInitStringModule())
		strcat(initError, "ERROR: Failed to init std.string module\r\n");

	if(!nullcInitCanvasModule())
		strcat(initError, "ERROR: Failed to init img.canvas module\r\n");
	if(!nullcInitWindowModule())
		strcat(initError, "ERROR: Failed to init win.window module\r\n");

	if(!nullcInitVectorModule())
		strcat(initError, "ERROR: Failed to init std.vector module\r\n");
	if(!nullcInitListModule())
		strcat(initError, "ERROR: Failed to init std.list module\r\n");
	if(!nullcInitMapModule())
		strcat(initError, "ERROR: Failed to init std.map module\r\n");
	if(!nullcInitHashmapModule())
		strcat(initError, "ERROR: Failed to init std.hashmap module\r\n");
	if(!nullcInitRandomModule())
		strcat(initError, "ERROR: Failed to init std.random module\r\n");
	if(!nullcInitTimeModule())
		strcat(initError, "ERROR: Failed to init std.time module\r\n");
	if(!nullcInitGCModule())
		strcat(initError, "ERROR: Failed to init std.gc module\r\n");

	nullcLoadModuleBySource("ide.debug", "void _debugBreak();");
	nullcBindModuleFunction("ide.debug", (void(*)())IDEDebugBreak, "_debugBreak", 0);

	colorer = NULL;

	// Initialize global strings
	LoadString(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
	LoadString(hInstance, IDC_SUPERCALC, szWindowClass, MAX_LOADSTRING);
	MyRegisterClass(hInstance);

	lastUpdate = 0;
	// Perform application initialization:
	if(!InitInstance(hInstance, nCmdShow)) 
		return 0;

	if(!nullcDebugSetBreakFunction(IDEDebugBreakEx))
		strcat(initError, nullcGetLastError());

	WORD wVersionRequested = MAKEWORD(2, 2);
	WSADATA wsaData;
	WSAStartup(wVersionRequested, &wsaData);

	// Get the local host information
	RemoteData::localHost = gethostbyname("localhost");//*/"192.168.0.163");
	RemoteData::localIP = inet_ntoa(*(struct in_addr *)*RemoteData::localHost->h_addr_list);

	// Set up the sockaddr structure
	RemoteData::saServer.sin_family = AF_INET;
	RemoteData::saServer.sin_addr.s_addr = inet_addr(RemoteData::localIP);
	RemoteData::saServer.sin_port = htons(7590);

	if(!InitializeCriticalSectionAndSpinCount(&pipeSection, 0x80000400))
		strcat(initError, "Failed to create critical section for remote debugging");

	HACCEL hAccelTable = LoadAccelerators(hInstance, (LPCTSTR)IDR_SHORTCUTS);

	// Main message loop:
	while(GetMessage(&msg, NULL, 0, 0))
	{
		if(!TranslateAccelerator(hWnd, hAccelTable, &msg)) 
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}
	delete colorer;
	DeleteCriticalSection(&pipeSection);

	nullcTerminate();

	return (int) msg.wParam;
}

WORD MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEX wcex;
	memset(&wcex, 0, sizeof(WNDCLASSEX));
	wcex.cbSize = sizeof(WNDCLASSEX); 
	wcex.style			= 0;
	wcex.lpfnWndProc	= (WNDPROC)WndProc;
	wcex.cbClsExtra		= 0;
	wcex.cbWndExtra		= 0;
	wcex.hInstance		= hInstance;
	wcex.hIcon			= LoadIcon(hInstance, (LPCTSTR)IDI_SUPERCALC);
	wcex.hCursor		= LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground	= (HBRUSH)(COLOR_WINDOW);
	wcex.lpszMenuName	= (LPCTSTR)IDC_SUPERCALC;
	wcex.lpszClassName	= szWindowClass;
	wcex.hIconSm		= LoadIcon(wcex.hInstance, (LPCTSTR)IDI_SMALL);

	return RegisterClassEx(&wcex);
}

void AddTabWithFile(const char* filename, HINSTANCE hInstance)
{
	char buf[1024];
	char *file = NULL;
	GetFullPathName(filename, 1024, buf, &file);

	FILE *startText = fopen(buf, "rb");
	char *fileContent = NULL;
	if(!startText)
		return;

	richEdits.push_back(CreateWindow("NULLCTEXT", NULL, WS_CHILD | WS_BORDER, 5, 25, areaWidth, areaHeight, hWnd, NULL, hInstance, NULL));
	TabbedFiles::AddTab(hTabs, buf, richEdits.back());
	ShowWindow(richEdits.back(), SW_HIDE);
	
	fseek(startText, 0, SEEK_END);
	unsigned int textSize = ftell(startText);
	fseek(startText, 0, SEEK_SET);
	fileContent = new char[textSize+1];
	fread(fileContent, 1, textSize, startText);
	fileContent[textSize] = 0;
	fclose(startText);

	RichTextarea::SetAreaText(richEdits.back(), fileContent ? fileContent : "");
	delete[] fileContent;

	RichTextarea::BeginStyleUpdate(richEdits.back());
	colorer->ColorText(richEdits.back(), (char*)RichTextarea::GetAreaText(richEdits.back()), RichTextarea::SetStyleToSelection);
	RichTextarea::EndStyleUpdate(richEdits.back());
	RichTextarea::UpdateArea(richEdits.back());
	RichTextarea::ResetUpdate(richEdits.back());
}

bool SaveFileFromTab(const char *file, const char *data)
{
	FILE *fSave = fopen(file, "wb");
	if(!fSave)
	{
		MessageBox(hWnd, "File cannot be saved", "Warning", MB_OK);
		return false;
	}else{
		fwrite(data, 1, strlen(data), fSave);
		fclose(fSave);
	}
	return true;
}

void CloseTabWithFile(TabbedFiles::TabInfo &info)
{
	if(info.dirty && MessageBox(hWnd, "File was changed. Save changes?", "Warning", MB_YESNO) == IDYES)
	{
		SaveFileFromTab(info.name, RichTextarea::GetAreaText(info.window));
	}
	DestroyWindow(info.window);
	for(unsigned int i = 0; i < richEdits.size(); i++)
	{
		if(richEdits[i] == info.window)
		{
			richEdits.erase(richEdits.begin() + i);
			break;
		}
	}
}

bool InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	hInst = hInstance; // Store instance handle in our global variable

	hWnd = CreateWindow(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW, 100, 100, 900, 450, NULL, NULL, hInstance, NULL);
	if(!hWnd)
		return 0;
	ShowWindow(hWnd, nCmdShow);
	UpdateWindow(hWnd);

	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hWnd, &ps);
	fontMonospace = CreateFont(-9 * GetDeviceCaps(hdc, LOGPIXELSY) / 72, 0, 0, 0, 0, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_DONTCARE, "Courier New");
	fontDefault = CreateFont(-10 * GetDeviceCaps(hdc, LOGPIXELSY) / 72, 0, 0, 0, 0, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_DONTCARE, "Arial");
	EndPaint(hWnd, &ps);

	hButtonCalc = CreateWindow("BUTTON", "Run", WS_VISIBLE | WS_CHILD, 5, 185, 100, 30, hWnd, NULL, hInstance, NULL);
	if(!hButtonCalc)
		return 0;
	SendMessage(hButtonCalc, WM_SETFONT, (WPARAM)fontDefault, 0);

	hContinue = CreateWindow("BUTTON", "Continue", WS_CHILD, 110, 185, 100, 30, hWnd, NULL, hInstance, NULL);
	if(!hContinue)
		return 0;
	SendMessage(hContinue, WM_SETFONT, (WPARAM)fontDefault, 0);

	hJITEnabled = CreateWindow("BUTTON", "X86 JIT", WS_VISIBLE | BS_AUTOCHECKBOX | WS_CHILD, 800-140, 185, 130, 30, hWnd, NULL, hInstance, NULL);
	if(!hJITEnabled)
		return 0;
	SendMessage(hJITEnabled, WM_SETFONT, (WPARAM)fontDefault, 0);

	INITCOMMONCONTROLSEX commControlTypes;
	commControlTypes.dwSize = sizeof(INITCOMMONCONTROLSEX);
	commControlTypes.dwICC = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES;
	int commControlsAvailable = InitCommonControlsEx(&commControlTypes);
	if(!commControlsAvailable)
		return 0;

	hStatus = CreateStatusWindow(WS_CHILD | WS_VISIBLE, "Ready", hWnd, 0);

	TabbedFiles::RegisterTabbedFiles("NULLCTABS", hInstance);
	RichTextarea::RegisterTextarea("NULLCTEXT", hInstance);

	colorer = new Colorer();

	hTabs = CreateWindow("NULLCTABS", "tabs", WS_VISIBLE | WS_CHILD, 5, 4, 800, 20, hWnd, 0, hInstance, 0);
	if(!hTabs)
		return 0;

	hAttachTabs = CreateWindow("NULLCTABS", "tabs", WS_CHILD, 5, 4, 800, 20, hWnd, 0, hInstance, 0);
	if(!hAttachTabs)
		return 0;

	// Load tab information
	FILE *tabInfo = fopen("nullc_tab.cfg", "rb");
	if(!tabInfo)
	{
		AddTabWithFile("main.nc", hInstance);
		if(richEdits.empty())
		{
			richEdits.push_back(CreateWindow("NULLCTEXT", NULL, WS_CHILD | WS_BORDER, 5, 25, areaWidth, areaHeight, hWnd, NULL, hInstance, NULL));
			TabbedFiles::AddTab(hTabs, "main.nc", richEdits.back());
			ShowWindow(richEdits.back(), SW_HIDE);
			RichTextarea::SetAreaText(richEdits.back(), "");
		}
	}else{
		fseek(tabInfo, 0, SEEK_END);
		unsigned int textSize = ftell(tabInfo);
		fseek(tabInfo, 0, SEEK_SET);
		char *fileContent = new char[textSize+1];
		fread(fileContent, 1, textSize, tabInfo);
		fileContent[textSize] = 0;
		fclose(tabInfo);

		char *start = fileContent;
		while(char *end = strchr(start, '\r'))
		{
			*end = 0;
			AddTabWithFile(start, hInstance);
			start = end + 2;
		}
		delete[] fileContent;
	}

	TabbedFiles::SetOnCloseTab(hTabs, CloseTabWithFile);
	TabbedFiles::SetNewTabWindow(hTabs, hNewTab = CreateWindow("STATIC", "", WS_CHILD | SS_GRAYFRAME, 5, 25, 780, 175, hWnd, NULL, hInstance, NULL));
	HWND createPanel = CreateWindow("STATIC", "", WS_VISIBLE | WS_CHILD, 5, 5, 190, 60, hNewTab, NULL, hInstance, NULL);
	/*HWND panel0 = */CreateWindow("STATIC", "", WS_VISIBLE | WS_CHILD | SS_ETCHEDFRAME, 0, 0, 190, 60, createPanel, NULL, hInstance, NULL);
	HWND panel1 = CreateWindow("STATIC", "File name: ", WS_VISIBLE | WS_CHILD, 5, 5, 100, 20, createPanel, NULL, hInstance, NULL);
	hNewFilename = CreateWindow("EDIT", "", WS_VISIBLE | WS_CHILD, 80, 5, 100, 20, createPanel, NULL, hInstance, NULL);
	hNewFile = CreateWindow("BUTTON", "Create", WS_VISIBLE | WS_CHILD, 5, 30, 100, 25, createPanel, NULL, hInstance, NULL);

	SetWindowLong(createPanel, GWLP_WNDPROC, (LONG)(intptr_t)WndProc);

	SendMessage(panel1, WM_SETFONT, (WPARAM)fontDefault, 0);
	SendMessage(hNewFilename, WM_SETFONT, (WPARAM)fontDefault, 0);
	SendMessage(hNewFile, WM_SETFONT, (WPARAM)fontDefault, 0);

	UpdateWindow(hTabs);

	if(!richEdits.empty())
		ShowWindow(richEdits[0], SW_SHOW);
	else
		ShowWindow(hNewTab, SW_SHOW);

	RichTextarea::SetStatusBar(hStatus, 900);

	RichTextarea::SetTextStyle(0,    0,   0,   0, false, false, false);
	RichTextarea::SetTextStyle(1,    0,   0, 255, false, false, false);
	RichTextarea::SetTextStyle(2,  128, 128, 128, false, false, false);
	RichTextarea::SetTextStyle(3,   50,  50,  50, false, false, false);
	RichTextarea::SetTextStyle(4,  136,   0,   0, false,  true, false);
	RichTextarea::SetTextStyle(5,    0,   0,   0, false, false, false);
	RichTextarea::SetTextStyle(6,    0,   0,   0,  true, false, false);
	RichTextarea::SetTextStyle(7,  136,   0,   0, false, false, false);
	RichTextarea::SetTextStyle(8,    0, 150,   0, false, false, false);
	RichTextarea::SetTextStyle(9,    0, 150,   0, false,  true, false);
	RichTextarea::SetTextStyle(10, 255,   0,   0, false, false,  true);
	RichTextarea::SetTextStyle(11, 255,   0, 255, false, false, false);

	RichTextarea::SetLineStyle(OVERLAY_CALLED, LoadBitmap(hInst, MAKEINTRESOURCE(IDB_CALL)), "This code has called into another function");
	RichTextarea::SetLineStyle(OVERLAY_STOP, LoadBitmap(hInst, MAKEINTRESOURCE(IDB_LASTCALL)), "Code execution stopped at this point");
	RichTextarea::SetLineStyle(OVERLAY_CURRENT, LoadBitmap(hInst, MAKEINTRESOURCE(IDB_CURR)), "Code execution is currently at this point");
	RichTextarea::SetLineStyle(OVERLAY_BREAKPOINT, LoadBitmap(hInst, MAKEINTRESOURCE(IDB_BREAK)), "Breakpoint");
	RichTextarea::SetLineStyle(OVERLAY_BREAKPOINT_INVALID, LoadBitmap(hInst, MAKEINTRESOURCE(IDB_UNREACHABLE)), "Breakpoint is never reached");

	unsigned int width = (800 - 25) / 4;

	hCode = CreateWindow("EDIT", initError, WS_CHILD | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_AUTOHSCROLL | ES_AUTOVSCROLL | ES_MULTILINE | ES_READONLY,
		5, 225, width*2, 165, hWnd, NULL, hInstance, NULL);
	if(!hCode)
		return 0;
	ShowWindow(hCode, nCmdShow);
	UpdateWindow(hCode);
	SendMessage(hCode, WM_SETFONT, (WPARAM)fontMonospace, 0);

	hVars = CreateWindow(WC_TREEVIEW, "", WS_CHILD | WS_BORDER | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_EDITLABELS,
		3*width+15, 225, width, 165, hWnd, NULL, hInstance, NULL);
	if(!hVars)
		return 0;
	ShowWindow(hVars, nCmdShow);
	UpdateWindow(hVars);

	hResult = CreateWindow("STATIC", "The result will be here", WS_CHILD, 110, 185, 300, 30, hWnd, NULL, hInstance, NULL);
	if(!hResult)
		return 0;
	ShowWindow(hResult, nCmdShow);
	UpdateWindow(hResult);
	SendMessage(hResult, WM_SETFONT, (WPARAM)fontDefault, 0);

	PostMessage(hWnd, WM_SIZE, 0, (394 << 16) + (900 - 16));

	hAttachPanel = CreateWindow("STATIC", "", WS_CHILD | SS_ETCHEDFRAME, 5, 5, 190, 60, hWnd, NULL, hInstance, NULL);
	hAttachList = CreateWindow(WC_LISTVIEW, "", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 10, 500, 400, hAttachPanel, NULL, hInstance, NULL);

	hAttachDo = CreateWindow("BUTTON", "Attach to process", WS_CHILD, 5, 185, 100, 30, hWnd, NULL, hInstance, NULL);
	SendMessage(hAttachDo, WM_SETFONT, (WPARAM)fontDefault, 0);

	hAttachBack = CreateWindow("BUTTON", "Cancel", WS_CHILD, 110, 185, 100, 30, hWnd, NULL, hInstance, NULL);
	SendMessage(hAttachBack, WM_SETFONT, (WPARAM)fontDefault, 0);

	ListView_SetExtendedListViewStyle(hAttachList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);// | LVS_SHOWSELALWAYS);

	LVCOLUMN lvColumn;
	lvColumn.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	lvColumn.iSubItem = 0;
	lvColumn.pszText = "Pipe";
	lvColumn.cx = 200;
	lvColumn.fmt = LVCFMT_LEFT;
	ListView_InsertColumn(hAttachList, 0, &lvColumn);

	lvColumn.iSubItem = 1;
	lvColumn.pszText = "Application";
	lvColumn.cx = 400;
	lvColumn.fmt = LVCFMT_LEFT;
	ListView_InsertColumn(hAttachList, 1, &lvColumn);

	lvColumn.iSubItem = 2;
	lvColumn.pszText = "PID";
	lvColumn.cx = 50;
	lvColumn.fmt = LVCFMT_LEFT;
	ListView_InsertColumn(hAttachList, 2, &lvColumn);

	for(unsigned int i = 0; i < 16; i++)
	{
		LVITEM lvItem;
		lvItem.mask = LVIF_TEXT | LVIF_STATE;
		lvItem.state = 0;
		lvItem.stateMask = 0;

		lvItem.iItem = i;
		lvItem.iSubItem = 0;
		lvItem.pszText = "";
		ListView_InsertItem(hAttachList, &lvItem);
	}

	SetTimer(hWnd, 1, 500, 0);

	return TRUE;
}

// zero-terminated safe sprintf
int	safeprintf(char* dst, size_t size, const char* src, ...)
{
	va_list args;
	va_start(args, src);

	int result = vsnprintf(dst, size, src, args);
	dst[size-1] = '\0';
	return result;
}

ExternVarInfo	*codeVars = NULL;
unsigned int	codeTypeCount = 0;
ExternTypeInfo	*codeTypes = NULL;
ExternFuncInfo	*codeFuntions = NULL;
ExternLocalInfo	*codeLocals = NULL;
unsigned int	*codeTypeExtra = NULL;
char			*codeSymbols = NULL;

struct TreeItemExtra
{
	TreeItemExtra(void* a, const ExternTypeInfo* t, HTREEITEM i):address(a), type(t), item(i){}

	void			*address;
	const ExternTypeInfo	*type;
	HTREEITEM		item;
};
std::vector<TreeItemExtra>	tiExtra;
std::vector<char*> externalBlocks;

const char* GetBasicVariableInfo(const ExternTypeInfo& type, char* ptr)
{
	static char val[256];

	switch(type.type)
	{
	case ExternTypeInfo::TYPE_CHAR:
		if(*(unsigned char*)ptr)
			safeprintf(val, 256, "'%c' (%d)", *(unsigned char*)ptr, (int)*(unsigned char*)ptr);
		else
			safeprintf(val, 256, "0");
		break;
	case ExternTypeInfo::TYPE_SHORT:
		safeprintf(val, 256, "%d", *(short*)ptr);
		break;
	case ExternTypeInfo::TYPE_INT:
		safeprintf(val, 256, type.subType == 0 ? "%d" : "0x%x", *(int*)ptr);
		break;
	case ExternTypeInfo::TYPE_LONG:
		safeprintf(val, 256, "%I64d", *(long long*)ptr);
		break;
	case ExternTypeInfo::TYPE_FLOAT:
		safeprintf(val, 256, "%f", *(float*)ptr);
		break;
	case ExternTypeInfo::TYPE_DOUBLE:
		safeprintf(val, 256, "%f", *(double*)ptr);
		break;
	default:
		safeprintf(val, 256, "not basic type");
	}
	return val;
}

void FillArrayVariableInfo(const ExternTypeInfo& type, char* ptr, HTREEITEM parent)
{
	TVINSERTSTRUCT helpInsert;
	helpInsert.hParent = parent;
	helpInsert.hInsertAfter = TVI_LAST;
	helpInsert.item.mask = TVIF_TEXT;
	helpInsert.item.cchTextMax = 0;

	char name[256];
	HTREEITEM lastItem;

	ExternTypeInfo	&subType = codeTypes[type.subType];
	unsigned int arrSize = (type.arrSize == ~0u) ? *(unsigned int*)(ptr + 4) : type.arrSize;
	if(type.arrSize == ~0u)
	{
		arrSize = *(unsigned int*)(ptr + 4);
		TVITEM item;
		item.mask = TVIF_PARAM;
		item.lParam = tiExtra.size();
		item.hItem = parent;
		tiExtra.push_back(TreeItemExtra((void*)ptr, &type, parent));
		TreeView_SetItem(hVars, &item);
		return;
	}
	for(unsigned int i = 0; i < arrSize; i++, ptr += subType.size)
	{
		if(i > 100)
			break;
		char *it = name;
		memset(name, 0, 256);

		it += safeprintf(it, 256 - int(it - name), "[%d]: ", i);

		if(subType.subCat == ExternTypeInfo::CAT_NONE || subType.subCat == ExternTypeInfo::CAT_POINTER)
			it += safeprintf(it, 256 - int(it - name), " %s", GetBasicVariableInfo(subType, ptr));
		else if(&subType == &codeTypes[8])	// for typeid
			it += safeprintf(it, 256 - int(it - name), " = %s", codeSymbols + codeTypes[*(int*)(ptr)].offsetToName);

		helpInsert.item.mask = TVIF_TEXT | TVIF_CHILDREN | TVIF_PARAM;
		helpInsert.item.pszText = name;
		helpInsert.item.cChildren = subType.subCat == ExternTypeInfo::CAT_POINTER ? I_CHILDRENCALLBACK : (subType.subCat == ExternTypeInfo::CAT_NONE ? 0 : 1);
		helpInsert.item.lParam = subType.subCat == ExternTypeInfo::CAT_POINTER ? tiExtra.size() : 0;

		HTREEITEM lastItem = TreeView_InsertItem(hVars, &helpInsert);
		if(subType.subCat == ExternTypeInfo::CAT_POINTER)
			tiExtra.push_back(TreeItemExtra((void*)ptr, &subType, lastItem));

		FillVariableInfo(subType, ptr, lastItem);
	}
	if(arrSize > 100)
	{
		safeprintf(name, 256, "[100]-[%d]...", 100, arrSize);
		helpInsert.item.pszText = name;
		lastItem = TreeView_InsertItem(hVars, &helpInsert);
	}
}

void FillComplexVariableInfo(const ExternTypeInfo& type, char* ptr, HTREEITEM parent)
{
	TVINSERTSTRUCT helpInsert;
	helpInsert.hParent = parent;
	helpInsert.hInsertAfter = TVI_LAST;
	helpInsert.item.mask = TVIF_TEXT;
	helpInsert.item.cchTextMax = 0;

	char name[256];

	const char *memberName = codeSymbols + type.offsetToName + (unsigned int)strlen(codeSymbols + type.offsetToName) + 1;
	for(unsigned int i = 0; i < type.memberCount; i++)
	{
		char *it = name;
		memset(name, 0, 256);

		ExternTypeInfo	&memberType = codeTypes[codeTypeExtra[type.memberOffset + i]];

		it += safeprintf(it, 256 - int(it - name), "%s %s", codeSymbols + memberType.offsetToName, memberName);

		if(memberType.subCat == ExternTypeInfo::CAT_NONE || memberType.subCat == ExternTypeInfo::CAT_POINTER)
			it += safeprintf(it, 256 - int(it - name), " = %s", GetBasicVariableInfo(memberType, ptr));
		else if(&memberType == &codeTypes[8])	// for typeid
			it += safeprintf(it, 256 - int(it - name), " = %s", codeSymbols + codeTypes[*(int*)(ptr)].offsetToName);

		helpInsert.item.mask = TVIF_TEXT | TVIF_CHILDREN | TVIF_PARAM;
		helpInsert.item.pszText = name;
		helpInsert.item.cChildren = memberType.subCat == ExternTypeInfo::CAT_POINTER ? I_CHILDRENCALLBACK : (memberType.subCat == ExternTypeInfo::CAT_NONE ? 0 : 1);
		helpInsert.item.lParam = memberType.subCat == ExternTypeInfo::CAT_POINTER ? tiExtra.size() : 0;

		HTREEITEM lastItem = TreeView_InsertItem(hVars, &helpInsert);
		if(memberType.subCat == ExternTypeInfo::CAT_POINTER)
			tiExtra.push_back(TreeItemExtra((void*)ptr, &memberType, lastItem));

		FillVariableInfo(memberType, ptr, lastItem);

		memberName += (unsigned int)strlen(memberName) + 1;
		ptr += memberType.size;	// $$ alignment?
	}
}

void FillAutoInfo(char* ptr, HTREEITEM parent)
{
	TVINSERTSTRUCT helpInsert;
	helpInsert.hParent = parent;
	helpInsert.hInsertAfter = TVI_LAST;
	helpInsert.item.mask = TVIF_TEXT;
	helpInsert.item.cchTextMax = 0;
	char name[256];

	safeprintf(name, 256, "typeid type = %d (%s)", *(int*)ptr, codeSymbols + codeTypes[*(int*)(ptr)].offsetToName);
	helpInsert.item.pszText = name;
	TreeView_InsertItem(hVars, &helpInsert);

	safeprintf(name, 256, "%s ref ptr = 0x%x", codeSymbols + codeTypes[*(int*)(ptr)].offsetToName, *(int*)(ptr + 4));

	// Find parent type
	ExternTypeInfo *parentType = NULL;
	for(unsigned int i = 0; i < codeTypeCount && !parentType; i++)
		if(codeTypes[i].subCat == ExternTypeInfo::CAT_POINTER && codeTypes[i].subType == *(unsigned int*)(ptr))
			parentType = &codeTypes[i];

	helpInsert.item.mask = TVIF_TEXT | TVIF_CHILDREN | TVIF_PARAM;
	helpInsert.item.pszText = name;
	helpInsert.item.cChildren = I_CHILDRENCALLBACK;
	helpInsert.item.lParam = tiExtra.size();

	HTREEITEM lastItem = TreeView_InsertItem(hVars, &helpInsert);
	tiExtra.push_back(TreeItemExtra((void*)(ptr + 4), parentType, lastItem));
}

void FillFunctionPointerInfo(const ExternTypeInfo& type, char* ptr, HTREEITEM parent)
{
	TVINSERTSTRUCT helpInsert;
	helpInsert.hParent = parent;
	helpInsert.hInsertAfter = TVI_LAST;
	helpInsert.item.mask = TVIF_TEXT;
	helpInsert.item.cchTextMax = 0;
	char name[256];

	if(nullcGetCurrentExecutor(NULL) == NULLC_X86)
		return;

	ExternFuncInfo	&func = codeFuntions[*(int*)(ptr + 4)];
	ExternTypeInfo	&returnType = codeTypes[codeTypeExtra[type.memberOffset]];

	char *it = name;
	it += safeprintf(it, 256 - int(it - name), "function %d %s %s(", *(int*)(ptr + 4), codeSymbols + returnType.offsetToName, codeSymbols + func.offsetToName);
	for(unsigned int arg = 0; arg < func.paramCount; arg++)
	{
		ExternLocalInfo &lInfo = codeLocals[func.offsetToFirstLocal + arg];
		it += safeprintf(it, 256 - int(it - name), "%s %s", codeSymbols + codeTypes[lInfo.type].offsetToName, codeSymbols + lInfo.offsetToName);
	}
	it += safeprintf(it, 256 - int(it - name), ")");

	helpInsert.item.pszText = name;
	TreeView_InsertItem(hVars, &helpInsert);

	safeprintf(name, 256, "void ref context = 0x%x", *(int*)(ptr));
	helpInsert.item.pszText = name;
	HTREEITEM contextList = TreeView_InsertItem(hVars, &helpInsert);

	TVINSERTSTRUCT nextInsert;
	nextInsert.hParent = contextList;
	nextInsert.hInsertAfter = TVI_LAST;
	nextInsert.item.mask = TVIF_TEXT;
	nextInsert.item.cchTextMax = 0;

	ExternFuncInfo::Upvalue *upvalue = *(ExternFuncInfo::Upvalue**)(ptr);

	ExternLocalInfo *externals = &codeLocals[func.offsetToFirstLocal + func.localCount];
	for(unsigned int i = 0; i < func.externCount; i++)
	{
		char *it = name;
		ExternTypeInfo &externType = codeTypes[externals[i].type];

		it += safeprintf(it, 256 - int(it - name), "%s %s", codeSymbols + externType.offsetToName, codeSymbols + externals[i].offsetToName);

		if(externType.subCat == ExternTypeInfo::CAT_NONE || externType.subCat == ExternTypeInfo::CAT_POINTER)
			it += safeprintf(it, 256 - int(it - name), " = %s", GetBasicVariableInfo(externType, (char*)upvalue->ptr));
		else if(&externType == &codeTypes[8])	// for typeid
			it += safeprintf(it, 256 - int(it - name), " = %s", codeSymbols + codeTypes[*(int*)(upvalue->ptr)].offsetToName);

		nextInsert.item.pszText = name;
		HTREEITEM lastItem = TreeView_InsertItem(hVars, &nextInsert);

		FillVariableInfo(externType, (char*)upvalue->ptr, lastItem);

		upvalue = (ExternFuncInfo::Upvalue*)((int*)upvalue + ((externals[i].size >> 2) < 3 ? 3 : 1 + (externals[i].size >> 2)));
	}
}

void FillVariableInfo(const ExternTypeInfo& type, char* ptr, HTREEITEM parent)
{
	if(&type == &codeTypes[8])	// typeid
		return;
	if(&type == &codeTypes[7])
	{
		FillAutoInfo(ptr, parent);	// auto ref
		return;
	}

	switch(type.subCat)
	{
	case ExternTypeInfo::CAT_FUNCTION:
		FillFunctionPointerInfo(type, ptr, parent);
		break;
	case ExternTypeInfo::CAT_CLASS:
		FillComplexVariableInfo(type, ptr, parent);
		break;
	case ExternTypeInfo::CAT_ARRAY:
		FillArrayVariableInfo(type, ptr, parent);
		break;
	}
}

void FillVariableInfoTree(bool lastIsCurrent = false)
{
	TreeView_DeleteAllItems(hVars);
	tiExtra.clear();
	tiExtra.push_back(TreeItemExtra(NULL, NULL, 0));

	TVINSERTSTRUCT	helpInsert;
	helpInsert.hParent = NULL;
	helpInsert.hInsertAfter = TVI_ROOT;
	helpInsert.item.mask = TVIF_TEXT;
	helpInsert.item.cchTextMax = 0;

	char	*data = stateRemote ? RemoteData::stackData : (char*)nullcGetVariableData(NULL);

	unsigned int	variableCount	= stateRemote ? RemoteData::varCount : 0;
	unsigned int	functionCount	= stateRemote ? RemoteData::funcCount : 0;
	codeTypeCount = stateRemote ? RemoteData::typeCount : 0;
	codeVars		= stateRemote ? RemoteData::vars : nullcDebugVariableInfo(&variableCount);
	codeTypes		= stateRemote ? RemoteData::types : nullcDebugTypeInfo(&codeTypeCount);
	codeFuntions	= stateRemote ? RemoteData::functions : nullcDebugFunctionInfo(&functionCount);
	codeLocals		= stateRemote ? RemoteData::locals : nullcDebugLocalInfo(NULL);
	codeTypeExtra	= stateRemote ? RemoteData::typeExtra : nullcDebugTypeExtraInfo(NULL);
	codeSymbols		= stateRemote ? RemoteData::symbols : nullcDebugSymbols(NULL);

	char name[256];
	unsigned int offset = 0;

	for(unsigned int i = 0; i < variableCount; i++)
	{
		ExternTypeInfo &type = codeTypes[codeVars[i].type];

		char *it = name;
		memset(name, 0, 256);
		it += safeprintf(it, 256 - int(it - name), "0x%x: %s %s", data + codeVars[i].offset, codeSymbols + type.offsetToName, codeSymbols + codeVars[i].offsetToName);

		if(type.subCat == ExternTypeInfo::CAT_NONE || type.subCat == ExternTypeInfo::CAT_POINTER)
			it += safeprintf(it, 256 - int(it - name), " = %s", GetBasicVariableInfo(type, data + codeVars[i].offset));
		else if(&type == &codeTypes[8])	// for typeid
			it += safeprintf(it, 256 - int(it - name), " = %s", codeSymbols + codeTypes[*(int*)(data + codeVars[i].offset)].offsetToName);

		helpInsert.item.mask = TVIF_TEXT | TVIF_CHILDREN | TVIF_PARAM;
		helpInsert.item.pszText = name;
		helpInsert.item.cChildren = type.subCat == ExternTypeInfo::CAT_POINTER ? I_CHILDRENCALLBACK : (type.subCat == ExternTypeInfo::CAT_NONE ? 0 : 1);
		helpInsert.item.lParam = type.subCat == ExternTypeInfo::CAT_POINTER ? tiExtra.size() : 0;

		HTREEITEM lastItem = TreeView_InsertItem(hVars, &helpInsert);
		if(type.subCat == ExternTypeInfo::CAT_POINTER)
			tiExtra.push_back(TreeItemExtra(data + codeVars[i].offset, &type, lastItem));

		FillVariableInfo(type, data + codeVars[i].offset, lastItem);

		if(codeVars[i].offset + type.size > offset)
			offset = codeVars[i].offset + type.size;
	}

	unsigned int codeLine = ~0u;

	unsigned int infoSize = stateRemote ? RemoteData::infoSize : 0;
	NULLCCodeInfo *codeInfo = stateRemote ? RemoteData::codeInfo : nullcDebugCodeInfo(&infoSize);

	unsigned int moduleSize = stateRemote ? RemoteData::moduleCount : 0;
	ExternModuleInfo *modules = stateRemote ? RemoteData::modules : nullcDebugModuleInfo(&moduleSize);

	unsigned int id = TabbedFiles::GetCurrentTab(stateRemote ? hAttachTabs : hTabs);
	const char *source = RichTextarea::GetAreaText(TabbedFiles::GetTabInfo(stateRemote ? hAttachTabs : hTabs, id).window);

	unsigned int csPos = 0;
	if(!stateRemote)
		nullcDebugBeginCallStack();
	while(int address = stateRemote ? RemoteData::callStack[csPos++] : nullcDebugGetStackFrame())
	{
		// Find corresponding function
		int funcID = -1;
		for(unsigned int i = 0; i < functionCount; i++)
		{
			if(address >= codeFuntions[i].address && address < (codeFuntions[i].address + codeFuntions[i].codeSize))
			{
				funcID = i;
			}
		}

		if(address != -1)
		{
			unsigned int line = 0;
			unsigned int i = address - 1;
			while((line < infoSize - 1) && (i >= codeInfo[line + 1].byteCodePos))
				line++;
			
			if(codeInfo[line].sourceOffset >= modules[moduleSize-1].sourceOffset)
			{
				codeLine = 0;
				const char *curr = source, *end = source + codeInfo[line].sourceOffset - modules[moduleSize-1].sourceOffset - modules[moduleSize-1].sourceSize;
				while(const char *next = strchr(curr, '\n'))
				{
					if(next > end)
						break;
					curr = next + 1;
					codeLine++;
				}
				RichTextarea::SetStyleToLine(TabbedFiles::GetTabInfo(stateRemote ? hAttachTabs : hTabs, id).window, codeLine, OVERLAY_CALLED);
			}
		}

		// If we are not in global scope
		if(funcID != -1)
		{
			ExternFuncInfo	&function = codeFuntions[funcID];

			// Align offset to the first variable (by 16 byte boundary)
			int alignOffset = (offset % 16 != 0) ? (16 - (offset % 16)) : 0;
			offset += alignOffset;

			char *it = name;
			it += safeprintf(it, 256 - int(it - name), "0x%x: function %s(", data + offset, codeSymbols + function.offsetToName);
			for(unsigned int arg = 0; arg < function.paramCount; arg++)
			{
				ExternLocalInfo &lInfo = codeLocals[function.offsetToFirstLocal + arg];
				it += safeprintf(it, 256 - int(it - name), "%s %s", codeSymbols + codeTypes[lInfo.type].offsetToName, codeSymbols + lInfo.offsetToName);
				if(arg != function.paramCount - 1)
					it += safeprintf(it, 256 - int(it - name), ", ");
			}
			it += safeprintf(it, 256 - int(it - name), ")");

			helpInsert.item.mask = TVIF_TEXT;
			helpInsert.item.pszText = name;
			HTREEITEM lastItem = TreeView_InsertItem(hVars, &helpInsert);

			unsigned int offsetToNextFrame = 4;
			// Check every function local
			for(unsigned int i = 0; i < function.localCount; i++)
			{
				// Get information about local
				ExternLocalInfo &lInfo = codeLocals[function.offsetToFirstLocal + i];

				char *it = name;
				it += safeprintf(it, 256, "0x%x: %s %s", data + offset + lInfo.offset, codeSymbols + codeTypes[lInfo.type].offsetToName, codeSymbols + lInfo.offsetToName);

				if(codeTypes[lInfo.type].subCat == ExternTypeInfo::CAT_NONE || codeTypes[lInfo.type].subCat == ExternTypeInfo::CAT_POINTER)
					it += safeprintf(it, 256 - int(it - name), " = %s", GetBasicVariableInfo(codeTypes[lInfo.type], data + offset + lInfo.offset));
				else if(lInfo.type == 8)	// for typeid
					it += safeprintf(it, 256 - int(it - name), " = %s", codeSymbols + codeTypes[*(int*)(data + offset + lInfo.offset)].offsetToName);

				TVINSERTSTRUCT localInfo;
				localInfo.hParent = lastItem;
				localInfo.hInsertAfter = TVI_LAST;
				localInfo.item.cchTextMax = 0;
				localInfo.item.mask = TVIF_TEXT | TVIF_CHILDREN | TVIF_PARAM;
				localInfo.item.pszText = name;
				localInfo.item.cChildren = codeTypes[lInfo.type].subCat == ExternTypeInfo::CAT_POINTER ? I_CHILDRENCALLBACK : (codeTypes[lInfo.type].subCat == ExternTypeInfo::CAT_NONE ? 0 : 1);
				localInfo.item.lParam = codeTypes[lInfo.type].subCat == ExternTypeInfo::CAT_POINTER ? tiExtra.size() : 0;

				HTREEITEM thisItem = TreeView_InsertItem(hVars, &localInfo);
				if(codeTypes[lInfo.type].subCat == ExternTypeInfo::CAT_POINTER)
					tiExtra.push_back(TreeItemExtra((void*)(data + offset + lInfo.offset), &codeTypes[lInfo.type], thisItem));

				FillVariableInfo(codeTypes[lInfo.type], data + offset + lInfo.offset, thisItem);

				if(lInfo.offset + lInfo.size > offsetToNextFrame)
					offsetToNextFrame = lInfo.offset + lInfo.size;
			}
			offset += offsetToNextFrame;
		}
	}

	HWND wnd = TabbedFiles::GetTabInfo(stateRemote ? hAttachTabs : hTabs, id).window;
	if(codeLine != ~0u)
	{
		RichTextarea::SetStyleToLine(wnd, codeLine, lastIsCurrent ? OVERLAY_CURRENT : OVERLAY_STOP);
		RichTextarea::ScrollToLine(wnd, codeLine);
	}
	RichTextarea::UpdateArea(wnd);
	RichTextarea::ResetUpdate(wnd);
}

struct RunResult
{
	bool	finished;
	nullres	result;
	double	time;
	HWND	wnd;
} runRes;
HANDLE calcThread = INVALID_HANDLE_VALUE;

DWORD WINAPI CalcThread(void* param)
{
	RunResult &rres = *(RunResult*)param;
	rres.finished = false;
	rres.result = false;
	double time = myGetPreciseTime();
	nullres goodRun = nullcRunFunction(NULL);
	rres.time = myGetPreciseTime() - time;
	rres.finished = true;
	rres.result = goodRun;
	SendMessage(rres.wnd, WM_USER + 1, 0, 0);
	ExitThread(goodRun);
}

void RefreshBreakpoints()
{
	unsigned int id = TabbedFiles::GetCurrentTab(stateRemote ? hAttachTabs : hTabs);
	HWND wnd = TabbedFiles::GetTabInfo(stateRemote ? hAttachTabs : hTabs, id).window;
	for(unsigned int i = 0; i < (stateRemote ? attachedEdits.size() : richEdits.size()); i++)
	{
		RichTextarea::LineIterator it = RichTextarea::GetFirstLine(stateRemote ? attachedEdits[i] : richEdits[i]);
		while(it.line)
		{
			if(it.GetExtra() == 1)
				it.SetStyle(OVERLAY_BREAKPOINT);
			if(it.GetExtra() == 2)
				it.SetStyle(OVERLAY_BREAKPOINT_INVALID);
			it.GoForward();
		}
	}
	RichTextarea::UpdateArea(wnd);
	RichTextarea::ResetUpdate(wnd);
}

unsigned int ConvertPositionToInstruction(unsigned int relPos, unsigned int infoSize, NULLCCodeInfo* codeInfo, unsigned int &sourceOffset)
{
	// Find instruction...
	unsigned int bestID = ~0u, lastDistance = ~0u;
	for(unsigned int infoID = 0; infoID < infoSize; infoID++)
	{
		if(codeInfo[infoID].sourceOffset >= relPos && (unsigned int)(codeInfo[infoID].sourceOffset - relPos) < lastDistance)
		{
			bestID = infoID;
			lastDistance = (unsigned int)(codeInfo[infoID].sourceOffset - relPos);
		}
	}
	if(bestID != ~0u)
	{
		sourceOffset = codeInfo[bestID].sourceOffset;
		return codeInfo[bestID].byteCodePos;
	}
	return ~0u;
}

unsigned int ConvertLineToInstruction(const char *source, unsigned int line, const char* fullSource, unsigned int infoSize, NULLCCodeInfo *codeInfo, unsigned int moduleSize, ExternModuleInfo *modules)
{
	// Find source code position for this line
	unsigned int origLine = line;
	const char *pos = source;
	while(line-- && NULL != (pos = strchr(pos, '\n')))
		pos++;
	if(pos)
	{
		// Get relative position
		unsigned int relPos = (unsigned int)(pos - source);
		// Find module, where this source code is contained
		unsigned int shiftToLastModule = modules[moduleSize-1].sourceOffset + modules[moduleSize-1].sourceSize;
		for(unsigned int module = 0; module < moduleSize; module++)
		{
			if(strcmp(source, fullSource + modules[module].sourceOffset) == 0)
			{
				relPos += modules[module].sourceOffset;
				shiftToLastModule = 0;
				break;
			}
		}
		// Move position to start of main module
		if(shiftToLastModule && strcmp(source, fullSource + modules[moduleSize-1].sourceOffset + modules[moduleSize-1].sourceSize) == 0)
			relPos += shiftToLastModule;
		unsigned int offset = ~0u;
		unsigned int instID = ConvertPositionToInstruction(relPos, infoSize, codeInfo, offset);
		if(offset != ~0u)
		{
			const char *pos = fullSource + offset;
			const char *start = pos;
			while(*start && start > fullSource)
				start--;
			start++;
			unsigned int realLine = 0;
			while(++realLine && NULL != (start = strchr(start, '\n')) && start < pos)
				start++;
			realLine--;
			if(realLine != origLine)
				return ~0u;
		}
		return instID;
	}
	return ~0u;
}

void SuperCalcSetBreakpoints()
{
	nullcDebugClearBreakpoints();
	unsigned int infoSize = 0;
	NULLCCodeInfo *codeInfo = nullcDebugCodeInfo(&infoSize);

	const char *fullSource = nullcDebugSource();

	unsigned int moduleSize = 0;
	ExternModuleInfo *modules = nullcDebugModuleInfo(&moduleSize);
	// Set all breakpoints
	for(unsigned int i = 0; i < richEdits.size(); i++)
	{
		RichTextarea::LineIterator it = RichTextarea::GetFirstLine(richEdits[i]);
		while(it.line)
		{
			if(it.GetExtra())
			{
				unsigned int inst = ConvertLineToInstruction(RichTextarea::GetCachedAreaText(richEdits[i]), it.number, fullSource, infoSize, codeInfo, moduleSize, modules);
				if(inst != ~0u)
				{
					it.SetExtra(EXTRA_BREAKPOINT);
					nullcDebugAddBreakpoint(inst);
				}else{
					it.SetExtra(EXTRA_BREAKPOINT_INVALID);
					printf("Failed to add breakpoint at line %d\r\n", it.number);
				}
			}
			it.GoForward();
		}
	}
}
void SuperCalcRun(bool debug)
{
	if(!runRes.finished)
	{
		TerminateThread(calcThread, 0);
		ShowWindow(hContinue, SW_HIDE);
		SetWindowText(hButtonCalc, "Run");
		runRes.finished = true;
		return;
	}
	unsigned int id = TabbedFiles::GetCurrentTab(hTabs);
	if(id == richEdits.size())
		return;
	HWND wnd = TabbedFiles::GetTabInfo(hTabs, id).window;
	const char *source = RichTextarea::GetAreaText(wnd);
	RichTextarea::ResetLineStyle(wnd);

	for(unsigned int i = 0; i < richEdits.size(); i++)
	{
		if(!TabbedFiles::GetTabInfo(hTabs, i).dirty)
			continue;
		if(SaveFileFromTab(TabbedFiles::GetTabInfo(hTabs, i).name, RichTextarea::GetAreaText(TabbedFiles::GetTabInfo(hTabs, i).window)))
		{
			TabbedFiles::GetTabInfo(hTabs, i).dirty = false;
			RichTextarea::ResetUpdate(TabbedFiles::GetTabInfo(hTabs, i).window);
			InvalidateRect(hTabs, NULL, true);
		}
	}
#ifndef _DEBUG
	FreeConsole();
#endif
	SetWindowText(hCode, "");
	SetWindowText(hResult, "");

	nullcSetExecutor(Button_GetCheck(hJITEnabled) ? NULLC_X86 : NULLC_VM);

	nullres good = nullcBuild(source);
	nullcSaveListing("asm.txt");

	if(!good)
	{
		SetWindowText(hCode, nullcGetLastError());
	}else{
		if(debug)
		{
			// Cache all source code in linear form
			for(unsigned int i = 0; i < richEdits.size(); i++)
				RichTextarea::GetAreaText(richEdits[i]);
			SuperCalcSetBreakpoints();
		}
		SetWindowText(hButtonCalc, "Abort");
		calcThread = CreateThread(NULL, 1024*1024, CalcThread, &runRes, NULL, 0);
	}
}

DWORD WINAPI PipeThread(void* param)
{
	PipeData data;

	while(!param)
	{
		int result = RemoteData::SocketReceive(RemoteData::sck, (char*)&data, sizeof(data), 5);
		if(!result || result == -1 || result != sizeof(data) || data.cmd != DEBUG_BREAK_HIT)
		{
			continue;
		}
		ShowWindow(hContinue, SW_SHOW);
		RefreshBreakpoints();

		data.cmd = DEBUG_BREAK_STACK;
		delete[] RemoteData::stackData;
		RemoteData::stackData = PipeReceiveResponce(data);
		RemoteData::stackSize = data.data.elemCount;

		data.cmd = DEBUG_BREAK_CALLSTACK;
		RemoteData::callStack = (unsigned int*)PipeReceiveResponce(data);
		RemoteData::callStackFrames = data.data.elemCount;

		if(RemoteData::stackData && RemoteData::callStack)
			FillVariableInfoTree(true);

		delete[] RemoteData::callStack;
		RemoteData::callStack = NULL;

		WaitForSingleObject(breakResponse, INFINITE);

		data.cmd = DEBUG_BREAK_CONTINUE;
		data.question = false;
		if(!PipeSendRequest(data))
			break;
	}
	
	ExitThread(0);
}

unsigned int PipeReuqestData(DebugCommand cmd, void **ptr)
{
	PipeData data;
	data.cmd = cmd;
	data.question = true;
	if(!PipeSendRequest(data))
	{
		MessageBox(hWnd, "Failed to send request through pipe", "Error", MB_OK);
		return 0;
	}
	*ptr = (ExternModuleInfo*)PipeReceiveResponce(data);
	if(!*ptr)
	{
		MessageBox(hWnd, "Failed to receive response through pipe", "Error", MB_OK);
		return 0;
	}
	return data.data.elemCount;
}

void PipeInit()
{
	using namespace RemoteData;

	// Start by retrieving source and module information
	moduleCount = PipeReuqestData(DEBUG_MODULE_INFO, (void**)&modules);
	PipeReuqestData(DEBUG_MODULE_NAMES, (void**)&moduleNames);
	char *moduleNamesTmp = moduleNames;
	PipeReuqestData(DEBUG_SOURCE_INFO, (void**)&sourceCode);
	infoSize = PipeReuqestData(DEBUG_CODE_INFO, (void**)&codeInfo);

	typeCount = PipeReuqestData(DEBUG_TYPE_INFO, (void**)&types);
	funcCount = PipeReuqestData(DEBUG_FUNCTION_INFO, (void**)&functions);
	varCount = PipeReuqestData(DEBUG_VARIABLE_INFO, (void**)&vars);
	localCount = PipeReuqestData(DEBUG_LOCAL_INFO, (void**)&locals);
	PipeReuqestData(DEBUG_TYPE_EXTRA_INFO, (void**)&typeExtra);
	PipeReuqestData(DEBUG_SYMBOL_INFO, (void**)&symbols);

	if(!modules || !moduleNames || !sourceCode || !codeInfo || !types || !functions || !vars || !locals || !typeExtra || !symbols)
		return;

	char message[1024], *pos = message;
	message[0] = 0;

	for(unsigned int i = 0; i < attachedEdits.size(); i++)
		DestroyWindow(attachedEdits[i]);
	attachedEdits.clear();
	for(unsigned int i = 0; i < moduleCount; i++)
	{
		modules[i].name = moduleNamesTmp;
		moduleNamesTmp += strlen(moduleNamesTmp) + 1;
		pos += safeprintf(pos, 1024 - (pos - message), "Received information about module '%s'\r\n", modules[i].name);

		attachedEdits.push_back(CreateWindow("NULLCTEXT", NULL, WS_VISIBLE | WS_CHILD | WS_BORDER, 5, 25, areaWidth, areaHeight, hWnd, NULL, hInst, NULL));
		TabbedFiles::AddTab(hAttachTabs, modules[i].name, attachedEdits.back());
		RichTextarea::SetAreaText(attachedEdits.back(), sourceCode + modules[i].sourceOffset);
		UpdateWindow(attachedEdits.back());
	}
	attachedEdits.push_back(CreateWindow("NULLCTEXT", NULL, WS_VISIBLE | WS_CHILD | WS_BORDER, 5, 25, areaWidth, areaHeight, hWnd, NULL, hInst, NULL));
	TabbedFiles::AddTab(hAttachTabs, "__last.nc", attachedEdits.back());
	RichTextarea::SetAreaText(attachedEdits.back(), sourceCode + modules[moduleCount-1].sourceOffset + modules[moduleCount-1].sourceSize);
	UpdateWindow(attachedEdits.back());

	SetWindowText(hCode, message);

	TabbedFiles::SetCurrentTab(hAttachTabs, moduleCount);
	ShowWindow(attachedEdits.back(), SW_SHOW);

	pipeThread = CreateThread(NULL, 1024*1024, PipeThread, NULL, NULL, 0);
}

void ContinueAfterBreak()
{
	HWND wnd = TabbedFiles::GetTabInfo(hTabs, TabbedFiles::GetCurrentTab(hTabs)).window;
	RichTextarea::ResetLineStyle(wnd);
	RefreshBreakpoints();
	RichTextarea::UpdateArea(wnd);
	RichTextarea::ResetUpdate(wnd);
	ShowWindow(hContinue, SW_HIDE);
	SetEvent(breakResponse);
}

namespace Broadcast
{
	int		broadcastWorkers[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	struct	BroadcastResult
	{
		char	address[512];
		char	message[1024];
		char	pid[16];
	} itemResult[8];
	bool	itemAvailable[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	HANDLE	broadcastThreads[8];
}

DWORD WINAPI	BroadcastThread(void *param)
{
	using namespace Broadcast;

	int ID = (*(int*)param) - 1;

	sockaddr_in saServer;
	saServer.sin_family = AF_INET;
	saServer.sin_addr.s_addr = inet_addr(RemoteData::localIP);
	saServer.sin_port = htons((u_short)(7590 + ID));
	safeprintf(itemResult[ID].address, 512, "localhost:%d", 5970 + ID);

	SOCKET sck = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(connect(sck, (SOCKADDR*)&saServer, sizeof(saServer)))
	{
		closesocket(sck);

		safeprintf(itemResult[ID].message, 1024, "%s", GetLastErrorDesc());
		safeprintf(itemResult[ID].pid, 16, "-");
		EnterCriticalSection(&pipeSection);
		broadcastWorkers[ID] = 0;
		itemAvailable[ID] = false;
		LeaveCriticalSection(&pipeSection);
	}else{
		PipeData data;
		data.cmd = DEBUG_REPORT_INFO;
		data.question = true;

		int result = RemoteData::SocketSend(sck, (char*)&data, sizeof(data), 2);
		if(result == -1)
		{
			safeprintf(itemResult[ID].message, 1024, "Failed to request information to socket '%s'\r\n", GetLastErrorDesc());
			safeprintf(itemResult[ID].pid, 16, "-");
			EnterCriticalSection(&pipeSection);
			broadcastWorkers[ID] = 0;
			itemAvailable[ID] = false;
			LeaveCriticalSection(&pipeSection);
			closesocket(sck);
			return 0;
		}

		result = RemoteData::SocketReceive(sck, (char*)&data, sizeof(data), 2);
		if(result == -1)
		{
			safeprintf(itemResult[ID].message, 1024, "Failed to receive information '%s'\r\n", GetLastErrorDesc());
			safeprintf(itemResult[ID].pid, 16, "-");
			EnterCriticalSection(&pipeSection);
			broadcastWorkers[ID] = 0;
			itemAvailable[ID] = false;
			LeaveCriticalSection(&pipeSection);
			closesocket(sck);
			return 0;
		}
		if(data.cmd == DEBUG_REPORT_INFO && !data.question)
		{
			safeprintf(itemResult[ID].message, 1024, "%s", data.report.module);
			safeprintf(itemResult[ID].pid, 16, "%d", data.report.pID);
			EnterCriticalSection(&pipeSection);
			broadcastWorkers[ID] = 0;
			itemAvailable[ID] = true;
			LeaveCriticalSection(&pipeSection);
		}
		closesocket(sck);
	}

	return 0;
}
LRESULT CALLBACK WndProc(HWND hWnd, unsigned int message, WPARAM wParam, LPARAM lParam)
{
	int wmId, wmEvent;
	PAINTSTRUCT ps;
	HDC hdc;

	char fileName[512];
	fileName[0] = 0;
	OPENFILENAME openData = { sizeof(OPENFILENAME), hWnd, NULL, "NULLC Files\0*.nc\0All Files\0*.*\0\0", NULL, 0, 0, fileName, 512,
		NULL, 0, NULL, NULL, OFN_ALLOWMULTISELECT | OFN_EXPLORER, 0, 0, 0, 0, 0, 0, NULL, 0, 0 };

	char	result[1024];

	__try
	{
		switch(message) 
		{
		case WM_CREATE:
			runRes.finished = true;
			runRes.wnd = hWnd;

			breakResponse = CreateEvent(NULL, false, false, "NULLC Debug Break Continue Event");
			break;
		case WM_DESTROY:
			if(hWnd == ::hWnd)
			{
				FILE *tabInfo = fopen("nullc_tab.cfg", "wb");
				for(unsigned int i = 0; i < richEdits.size(); i++)
				{
					fprintf(tabInfo, "%s\r\n", TabbedFiles::GetTabInfo(hTabs, i).name);
					DestroyWindow(richEdits[i]);
				}
				for(unsigned int i = 0; i < attachedEdits.size(); i++)
					DestroyWindow(attachedEdits[i]);
				fclose(tabInfo);
				RichTextarea::UnregisterTextarea();
				PostQuitMessage(0);
			}
			break;
		case WM_USER + 1:
			SetWindowText(hButtonCalc, "Run");

			if(runRes.result)
			{
				const char *val = nullcGetResult();

				_snprintf(result, 1024, "The answer is: %s [in %f]", val, runRes.time);
				result[1023] = '\0';
				SetWindowText(hResult, result);

				RefreshBreakpoints();
				FillVariableInfoTree();
			}else{
				_snprintf(result, 1024, "%s", nullcGetLastError());
				result[1023] = '\0';
				SetWindowText(hCode, result);

				FillVariableInfoTree();
			}
			break;
		case WM_USER + 2:
			ShowWindow(hContinue, SW_SHOW);
			RefreshBreakpoints();
			FillVariableInfoTree(true);

			break;
		case WM_NOTIFY:
			if(((LPNMHDR)lParam)->code == TVN_GETDISPINFO && ((LPNMHDR)lParam)->hwndFrom == hVars)
			{
				LPNMTVDISPINFO info = (LPNMTVDISPINFO)lParam;
				if(info->item.mask & TVIF_CHILDREN)
				{
					if(codeTypes && info->item.lParam && tiExtra[info->item.lParam].type->subCat == ExternTypeInfo::CAT_POINTER)
						info->item.cChildren = 1;
				}
			}else if(((LPNMHDR)lParam)->code == TVN_ITEMEXPANDING && ((LPNMHDR)lParam)->hwndFrom == hVars){
				LPNMTREEVIEW info = (LPNMTREEVIEW)lParam;

				TreeItemExtra *extra = info->itemNew.lParam ? &tiExtra[info->itemNew.lParam] : NULL;
				if(extra)
				{
					codeTypes = stateRemote ? RemoteData::types : nullcDebugTypeInfo(NULL);
					char *ptr = extra->type->subCat == ExternTypeInfo::CAT_POINTER ? *(char**)extra->address : (char*)extra->address;
					const ExternTypeInfo &type = extra->type->subCat == ExternTypeInfo::CAT_POINTER ? codeTypes[extra->type->subType] : *extra->type;

					if(stateRemote)
					{
						PipeData data;
						data.cmd = DEBUG_BREAK_DATA;
						data.question = true;
						data.data.wholeSize = extra->type->subCat == ExternTypeInfo::CAT_POINTER ? type.size : ((NullCArray*)extra->address)->len * codeTypes[type.subType].size;
						data.data.elemCount = 0;
						data.data.dataSize = (unsigned int)(intptr_t)(extra->type->subCat == ExternTypeInfo::CAT_POINTER ? ptr : ((NullCArray*)extra->address)->ptr);
						if(!PipeSendRequest(data))
						{
							MessageBox(hWnd, "Failed to send request through pipe", "Error", MB_OK);
							return 0;
						}
						ptr = PipeReceiveResponce(data);
						if(!ptr)
						{
							MessageBox(hWnd, "Failed to send request through pipe", "Error", MB_OK);
							break;
						}
						if(!data.data.elemCount)
							break;
					}
					if(IsBadReadPtr(ptr, extra->type->size))
						break;
					// Delete item children
					while(HTREEITEM child = TreeView_GetChild(hVars, extra->item))
						TreeView_DeleteItem(hVars, child);
					if(extra->type->subCat == ExternTypeInfo::CAT_ARRAY)
					{
						ExternTypeInfo decoy = type;
						decoy.arrSize = ((NullCArray*)extra->address)->len;
						FillArrayVariableInfo(decoy, stateRemote ? ptr : ((NullCArray*)extra->address)->ptr, extra->item);
						if(stateRemote)
							externalBlocks.push_back(ptr);
						break;
					}
					char name[256];

					char *it = name;
					memset(name, 0, 256);
					it += safeprintf(it, 256 - int(it - name), "0x%x: %s ###", ptr, codeSymbols + type.offsetToName);

					if(type.subCat == ExternTypeInfo::CAT_NONE || type.subCat == ExternTypeInfo::CAT_POINTER)
						it += safeprintf(it, 256 - int(it - name), " = %s", GetBasicVariableInfo(type, ptr));
					else if(&type == &codeTypes[8])	// for typeid
						it += safeprintf(it, 256 - int(it - name), " = %s", codeSymbols + codeTypes[*(int*)ptr].offsetToName);

					TVINSERTSTRUCT helpInsert;
					helpInsert.hParent = extra->item;
					helpInsert.hInsertAfter = TVI_LAST;
					helpInsert.item.cchTextMax = 0;
					helpInsert.item.mask = TVIF_TEXT | TVIF_CHILDREN | TVIF_PARAM;
					helpInsert.item.pszText = name;
					helpInsert.item.cChildren = type.subCat == ExternTypeInfo::CAT_POINTER ? I_CHILDRENCALLBACK : (type.subCat == ExternTypeInfo::CAT_NONE ? 0 : 1);
					helpInsert.item.lParam = type.subCat == ExternTypeInfo::CAT_POINTER ? tiExtra.size() : 0;

					HTREEITEM lastItem = TreeView_InsertItem(hVars, &helpInsert);
					if(type.subCat == ExternTypeInfo::CAT_POINTER)
						tiExtra.push_back(TreeItemExtra(ptr, &type, lastItem));

					FillVariableInfo(type, ptr, lastItem);

					if(stateRemote)
						externalBlocks.push_back(ptr);
				}
			}
			break;
		case WM_COMMAND:
			wmId	= LOWORD(wParam);
			wmEvent = HIWORD(wParam);

			if((HWND)lParam == hContinue)
			{
				ContinueAfterBreak();
			}else if((HWND)lParam == hButtonCalc){
				SuperCalcRun(true);
			}else if((HWND)lParam == hNewFile){
				GetWindowText(hNewFilename, fileName, 512);
				SetWindowText(hNewFilename, "");
				if(!strstr(fileName, ".nc"))
					strcat(fileName, ".nc");
				// Check if file is already opened
				for(unsigned int i = 0; i < richEdits.size(); i++)
				{
					if(strcmp(TabbedFiles::GetTabInfo(hTabs, i).name, fileName) == 0)
					{
						TabbedFiles::SetCurrentTab(hTabs, i);
						return 0;
					}
				}
				FILE *fNew = fopen(fileName, "rb");

				char *filePart = NULL;
				GetFullPathName(fileName, 512, result, &filePart);
				if(fNew)
				{
					int action = MessageBox(hWnd, "File already exists, overwrite?", "Warning", MB_YESNOCANCEL);
					if(action == IDYES)
					{
						richEdits.push_back(CreateWindow("NULLCTEXT", NULL, WS_VISIBLE | WS_CHILD | WS_BORDER, 5, 25, areaWidth, areaHeight, ::hWnd, NULL, hInst, NULL));
						TabbedFiles::AddTab(hTabs, result, richEdits.back());
						TabbedFiles::SetCurrentTab(hTabs, (int)richEdits.size() - 1);
						TabbedFiles::GetTabInfo(hTabs, (int)richEdits.size() - 1).dirty = true;
					}else if(action == IDNO){
						fclose(fNew);
						AddTabWithFile(fileName, hInst);
						TabbedFiles::SetCurrentTab(hTabs, (int)richEdits.size() - 1);
					}
					fclose(fNew);
				}else{
					richEdits.push_back(CreateWindow("NULLCTEXT", NULL, WS_VISIBLE | WS_CHILD | WS_BORDER, 5, 25, areaWidth, areaHeight, ::hWnd, NULL, hInst, NULL));
					TabbedFiles::AddTab(hTabs, result, richEdits.back());
					TabbedFiles::SetCurrentTab(hTabs, (int)richEdits.size() - 1);
					TabbedFiles::GetTabInfo(hTabs, (int)richEdits.size() - 1).dirty = true;
				}
			}else if((HWND)lParam == hAttachBack){
				ShowWindow(hTabs, SW_SHOW);
				ShowWindow(hButtonCalc, SW_SHOW);
				ShowWindow(hResult, SW_SHOW);
				ShowWindow(hJITEnabled, SW_SHOW);
				ShowWindow(TabbedFiles::GetTabInfo(hTabs, TabbedFiles::GetCurrentTab(hTabs)).window, SW_SHOW);

				ShowWindow(hAttachPanel, SW_HIDE);
				ShowWindow(hAttachDo, SW_HIDE);
				ShowWindow(hAttachBack, SW_HIDE);
				ShowWindow(hAttachTabs, SW_HIDE);

				ShowWindow(hContinue, SW_HIDE);

				if(stateRemote)
				{
					// Kill debug tracking
					TerminateThread(pipeThread, 0);
					// Send debug detach and continue commands
					PipeData data;
					data.cmd = DEBUG_DETACH;
					data.question = false;
					if(!PipeSendRequest(data))
						MessageBox(hWnd, "Failed to send request through pipe", "Error", MB_OK);
					closesocket(RemoteData::sck);
					// Remove text area windows
					for(unsigned int i = 0; i < attachedEdits.size(); i++)
					{
						TabbedFiles::RemoveTab(hAttachTabs, 0);
						DestroyWindow(attachedEdits[i]);
					}
					// Destroy data
					delete[] RemoteData::modules;
					RemoteData::modules = NULL;
					delete[] RemoteData::moduleNames;
					RemoteData::moduleNames = NULL;
					delete[] RemoteData::codeInfo;
					RemoteData::codeInfo = NULL;
					delete[] RemoteData::sourceCode;
					RemoteData::sourceCode = NULL;
					delete[] RemoteData::vars;
					RemoteData::vars = NULL;
					delete[] RemoteData::types;
					RemoteData::types = NULL;
					delete[] RemoteData::functions;
					RemoteData::functions = NULL;
					delete[] RemoteData::locals;
					RemoteData::locals = NULL;
					delete[] RemoteData::typeExtra;
					RemoteData::typeExtra = NULL;
					delete[] RemoteData::symbols;
					RemoteData::symbols = NULL;
					delete[] RemoteData::stackData;
					RemoteData::stackData = NULL;

					for(unsigned int i = 0; i < externalBlocks.size(); i++)
						delete[] externalBlocks[i];
					externalBlocks.clear();

					codeVars = NULL;
					codeTypes = NULL;
					codeFuntions = NULL;
					codeLocals = NULL;
					codeTypeExtra = NULL;
					codeSymbols = NULL;

					stateRemote = false;
				}
				HMENU debugMenu = GetSubMenu(GetMenu(hWnd), 1);
				EnableMenuItem(debugMenu, ID_RUN, MF_BYCOMMAND | MF_ENABLED);
				EnableMenuItem(debugMenu, ID_DEBUG_ATTACHTOPROCESS, MF_BYCOMMAND | MF_ENABLED);
				stateAttach = false;
			}else if((HWND)lParam == hAttachDo){
				unsigned int ID = ListView_GetSelectionMark(hAttachList);
				RemoteData::saServer.sin_port = htons((u_short)(7590 + ID));
				RemoteData::sck = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				if(connect(RemoteData::sck, (SOCKADDR*)&RemoteData::saServer, sizeof(RemoteData::saServer)))
				{
					closesocket(RemoteData::sck);
					MessageBox(hWnd, "Cannot attach debugger to selected process", "ERROR", MB_OK);
				}else{
					ShowWindow(hAttachPanel, SW_HIDE);
					ShowWindow(hAttachDo, SW_HIDE);
					ShowWindow(hAttachTabs, SW_SHOW);

					stateAttach = false;
					stateRemote = true;
					PipeInit();
				}
			}
			// Parse the menu selections:
			switch (wmId)
			{
			case IDM_EXIT:
				DestroyWindow(hWnd);
				break;
			case ID_FILE_LOAD:
				GetCurrentDirectory(512, result + 512);
				if(GetOpenFileName(&openData))
				{
					const char *file = fileName;
					const char *path = fileName;
					const char *separator = "\\";
					// Skip path
					file += strlen(file) + 1;
					// Single file isn't divided by path\file, so step back
					if(!*file)
					{
						path = "";
						separator = "";
						file = fileName;
					}
					// For all files
					while(*file)
					{
						strcpy(result, path);
						strcat(result, separator);
						strcat(result, file);

						bool opened = false;
						// Check if file is already opened
						for(unsigned int i = 0; i < richEdits.size(); i++)
						{
							if(_stricmp(TabbedFiles::GetTabInfo(hTabs, i).name, result) == 0)
							{
								TabbedFiles::SetCurrentTab(hTabs, i);
								opened = true;
								break;
							}
						}
						if(!opened)
						{
							AddTabWithFile(result, hInst);
							TabbedFiles::SetCurrentTab(hTabs, (int)richEdits.size() - 1);
						}
						file += strlen(file) + 1;
					}
				}
				SetCurrentDirectory(result + 512);
				break;
			case ID_FILE_SAVE:
				{
					unsigned int id = TabbedFiles::GetCurrentTab(hTabs);
					if(SaveFileFromTab(TabbedFiles::GetTabInfo(hTabs, id).name, RichTextarea::GetAreaText(TabbedFiles::GetTabInfo(hTabs, id).window)))
					{
						TabbedFiles::GetTabInfo(hTabs, id).dirty = false;
						RichTextarea::ResetUpdate(TabbedFiles::GetTabInfo(hTabs, id).window);
						InvalidateRect(hTabs, NULL, true);
					}
				}
				break;
			case ID_FILE_SAVEALL:
				for(unsigned int i = 0; i < richEdits.size(); i++)
				{
					if(SaveFileFromTab(TabbedFiles::GetTabInfo(hTabs, i).name, RichTextarea::GetAreaText(TabbedFiles::GetTabInfo(hTabs, i).window)))
					{
						TabbedFiles::GetTabInfo(hTabs, i).dirty = false;
						RichTextarea::ResetUpdate(TabbedFiles::GetTabInfo(hTabs, i).window);
						InvalidateRect(hTabs, NULL, true);
					}
				}
				break;
			case ID_TOGGLE_BREAK:
				{
					unsigned int id = TabbedFiles::GetCurrentTab(stateRemote ? hAttachTabs : hTabs);
					HWND wnd = TabbedFiles::GetTabInfo(stateRemote ? hAttachTabs : hTabs, id).window;
					unsigned int line = RichTextarea::GetCurrentLine(wnd);

					RichTextarea::LineIterator it = RichTextarea::GetLine(wnd, line);
					bool breakSet = it.GetExtra() == 0;
					// If breakpoint is not found, add one
					if(breakSet)
					{
						RichTextarea::SetStyleToLine(wnd, line, OVERLAY_BREAKPOINT);
						RichTextarea::SetLineExtra(wnd, line, EXTRA_BREAKPOINT);
					}else{
						RichTextarea::SetStyleToLine(wnd, line, OVERLAY_NONE);
						RichTextarea::SetLineExtra(wnd, line, EXTRA_NONE);
					}

					if(!runRes.finished)
					{
						unsigned int infoSize = 0;
						NULLCCodeInfo *codeInfo = nullcDebugCodeInfo(&infoSize);
						const char *fullSource = nullcDebugSource();
						unsigned int moduleSize = 0;
						ExternModuleInfo *modules = nullcDebugModuleInfo(&moduleSize);
						unsigned int inst = ConvertLineToInstruction(RichTextarea::GetCachedAreaText(wnd), line, fullSource, infoSize, codeInfo, moduleSize, modules);
						if(inst != ~0u)
						{
							if(breakSet)
								nullcDebugAddBreakpoint(inst);
							else
								nullcDebugRemoveBreakpoint(inst);
						}else{
							if(breakSet)
							{
								RichTextarea::SetStyleToLine(wnd, line, OVERLAY_BREAKPOINT_INVALID);
								RichTextarea::SetLineExtra(wnd, line, EXTRA_BREAKPOINT_INVALID);
							}
						}
					}
					RichTextarea::UpdateArea(wnd);
					RichTextarea::ResetUpdate(wnd);

					if(stateRemote)
					{
						unsigned int inst = ConvertLineToInstruction(RichTextarea::GetAreaText(wnd), line, RemoteData::sourceCode, RemoteData::infoSize, RemoteData::codeInfo, RemoteData::moduleCount, RemoteData::modules);
						if(inst != ~0u)
						{
							PipeData data;
							data.cmd = DEBUG_BREAK_SET;
							data.question = true;
							data.debug.breakInst = inst;
							data.debug.breakSet = breakSet;
							if(!PipeSendRequest(data))
								MessageBox(hWnd, "Failed to send request through pipe", "Error", MB_OK);
						}else{
							if(breakSet)
							{
								RichTextarea::SetStyleToLine(wnd, line, OVERLAY_BREAKPOINT_INVALID);
								RichTextarea::SetLineExtra(wnd, line, EXTRA_BREAKPOINT_INVALID);
							}
						}
					}
				}
				break;
			case ID_RUN_DEBUG:
				if(!stateRemote && !stateAttach && runRes.finished)
					SuperCalcRun(true);
				if(stateRemote || !runRes.finished)
					ContinueAfterBreak();
				break;
			case ID_RUN:
				if(!stateRemote && !stateAttach && runRes.finished)
					SuperCalcRun(false);
				if(stateRemote || !runRes.finished)
					ContinueAfterBreak();
				break;
			case ID_DEBUG_ATTACHTOPROCESS:
			{
				ShowWindow(hTabs, SW_HIDE);
				ShowWindow(hButtonCalc, SW_HIDE);
				ShowWindow(hResult, SW_HIDE);
				ShowWindow(hJITEnabled, SW_HIDE);
				ShowWindow(TabbedFiles::GetTabInfo(hTabs, TabbedFiles::GetCurrentTab(hTabs)).window, SW_HIDE);

				ShowWindow(hAttachPanel, SW_SHOW);
				ShowWindow(hAttachDo, SW_SHOW);
				ShowWindow(hAttachBack, SW_SHOW);

				Button_Enable(hAttachDo, false);

				HMENU debugMenu = GetSubMenu(GetMenu(hWnd), 1);
				EnableMenuItem(debugMenu, ID_RUN, MF_BYCOMMAND | MF_GRAYED);
				EnableMenuItem(debugMenu, ID_DEBUG_ATTACHTOPROCESS, MF_BYCOMMAND | MF_GRAYED);

				stateAttach = true;
			}
				break;
			default:
				return DefWindowProc(hWnd, message, wParam, lParam);
			}
			break;
		
		case WM_PAINT:
			{
				hdc = BeginPaint(hWnd, &ps);
				EndPaint(hWnd, &ps);
			}
			break;
		case WM_TIMER:
		{
			if(stateAttach)
			{
				int selected = ListView_GetSelectionMark(hAttachList);

				LVITEM lvItem;
				lvItem.mask = LVIF_TEXT | LVIF_STATE;
				lvItem.state = 0;
				lvItem.stateMask = 0;
				for(unsigned int i = 0; i < 8; i++)
				{
					EnterCriticalSection(&pipeSection);
					if(!Broadcast::broadcastWorkers[i])
					{
						lvItem.iItem = i;
						lvItem.iSubItem = 0;
						lvItem.pszText = Broadcast::itemResult[i].address;
						ListView_SetItem(hAttachList, &lvItem);
						lvItem.iSubItem = 1;
						lvItem.pszText = Broadcast::itemResult[i].message;
						ListView_SetItem(hAttachList, &lvItem);
						lvItem.iSubItem = 2;
						lvItem.pszText = Broadcast::itemResult[i].pid;
						ListView_SetItem(hAttachList, &lvItem);
						Broadcast::broadcastWorkers[i] = i + 1;
						Broadcast::broadcastThreads[i] = CreateThread(NULL, 64*1024, BroadcastThread, &Broadcast::broadcastWorkers[i], NULL, 0);
					}
					LeaveCriticalSection(&pipeSection);
				}
				if(selected != -1)
					Button_Enable(hAttachDo, Broadcast::itemAvailable[selected]);
				ListView_SetSelectionMark(hAttachList, selected);
			}

			for(unsigned int i = 0; i < richEdits.size(); i++)
			{
				if(!TabbedFiles::GetTabInfo(hTabs, i).dirty && RichTextarea::NeedUpdate(TabbedFiles::GetTabInfo(hTabs, i).window))
				{
					TabbedFiles::GetTabInfo(hTabs, i).dirty = true;
					InvalidateRect(hTabs, NULL, false);
				}
			}

			unsigned int id = TabbedFiles::GetCurrentTab(hTabs);
			EnableMenuItem(GetMenu(hWnd), ID_FILE_SAVE, TabbedFiles::GetTabInfo(hTabs, id).dirty ? MF_ENABLED : MF_DISABLED);

			HWND wnd = stateRemote ? TabbedFiles::GetTabInfo(hAttachTabs, TabbedFiles::GetCurrentTab(hAttachTabs)).window : TabbedFiles::GetTabInfo(hTabs, id).window;
			if(!RichTextarea::NeedUpdate(wnd) || (GetTickCount()-lastUpdate < 100))
				break;
			SetWindowText(hCode, "");

			RichTextarea::BeginStyleUpdate(wnd);
			if(!colorer->ColorText(wnd, (char*)RichTextarea::GetAreaText(wnd), RichTextarea::SetStyleToSelection))
			{
				SetWindowText(hCode, colorer->GetError().c_str());
			}
			RichTextarea::EndStyleUpdate(wnd);
			RichTextarea::UpdateArea(wnd);
			RichTextarea::ResetUpdate(wnd);
			needTextUpdate = false;
			lastUpdate = GetTickCount();
		}
			break;
		case WM_GETMINMAXINFO:
		{
			MINMAXINFO	*info = (MINMAXINFO*)lParam;
			info->ptMinTrackSize.x = 400;
			info->ptMinTrackSize.y = 300;
		}
			break;
		case WM_SIZE:
		{
			unsigned int width = LOWORD(lParam), height = HIWORD(lParam);
			unsigned int mainPadding = 5, subPadding = 2;

			unsigned int middleHeight = 30;
			unsigned int heightTopandBottom = height - mainPadding * 2 - middleHeight - subPadding * 2;

			unsigned int topHeight = int(heightTopandBottom / 100.0 * 60.0);	// 60 %
			unsigned int bottomHeight = int(heightTopandBottom / 100.0 * 40.0);	// 40 %

			unsigned int middleOffsetY = mainPadding + topHeight + subPadding;

			unsigned int tabHeight = 20;
			SetWindowPos(hTabs,			HWND_TOP, mainPadding, 4, width - mainPadding * 2, tabHeight, NULL);
			SetWindowPos(hAttachTabs,	HWND_TOP, mainPadding, 4, width - mainPadding * 2, tabHeight, NULL);

			areaWidth = width - mainPadding * 2;
			areaHeight = topHeight - tabHeight;
			for(unsigned int i = 0; i < richEdits.size(); i++)
				SetWindowPos(richEdits[i],	HWND_TOP, mainPadding, mainPadding + tabHeight, width - mainPadding * 2, topHeight - tabHeight, NULL);
			for(unsigned int i = 0; i < attachedEdits.size(); i++)
				SetWindowPos(attachedEdits[i],	HWND_TOP, mainPadding, mainPadding + tabHeight, width - mainPadding * 2, topHeight - tabHeight, NULL);
			SetWindowPos(hNewTab,		HWND_TOP, mainPadding, mainPadding + tabHeight, width - mainPadding * 2, topHeight - tabHeight, NULL);
			SetWindowPos(hAttachPanel,	HWND_TOP, mainPadding, mainPadding, width - mainPadding * 2, topHeight, NULL);

			SetWindowPos(hAttachList,	HWND_TOP, mainPadding, mainPadding, width - mainPadding * 4, topHeight - mainPadding * 2, NULL);

			unsigned int buttonWidth = 120;
			unsigned int resultWidth = width - 3 * buttonWidth - 3 * mainPadding - subPadding * 3;

			unsigned int calcOffsetX = mainPadding;
			unsigned int resultOffsetX = calcOffsetX * 2 + buttonWidth * 2 + subPadding;
			unsigned int x86OffsetX = resultOffsetX + resultWidth + subPadding;

			SetWindowPos(hButtonCalc,	HWND_TOP, calcOffsetX, middleOffsetY, buttonWidth, middleHeight, NULL);
			SetWindowPos(hResult,		HWND_TOP, resultOffsetX, middleOffsetY, resultWidth, middleHeight, NULL);
			SetWindowPos(hContinue,		HWND_TOP, calcOffsetX * 2 + buttonWidth, middleOffsetY, buttonWidth, middleHeight, NULL);
			SetWindowPos(hJITEnabled,	HWND_TOP, x86OffsetX, middleOffsetY, buttonWidth, middleHeight, NULL);

			SetWindowPos(hAttachDo,		HWND_TOP, calcOffsetX, middleOffsetY, buttonWidth, middleHeight, NULL);
			SetWindowPos(hAttachBack,	HWND_TOP, x86OffsetX, middleOffsetY, buttonWidth, middleHeight, NULL);

			unsigned int bottomOffsetY = middleOffsetY + middleHeight + subPadding;

			unsigned int bottomWidth = width - 2 * mainPadding - 2 * subPadding;
			unsigned int leftOffsetX = mainPadding;
			unsigned int leftWidth = int(bottomWidth / 100.0 * 75.0);	// 75 %
			unsigned int rightOffsetX = leftOffsetX + leftWidth + subPadding;
			unsigned int rightWidth = int(bottomWidth / 100.0 * 25.0);	// 25 %

			SetWindowPos(hCode,			HWND_TOP, leftOffsetX, bottomOffsetY, leftWidth, bottomHeight-16, NULL);
			SetWindowPos(hVars,			HWND_TOP, rightOffsetX, bottomOffsetY, rightWidth, bottomHeight-16, NULL);

			SetWindowPos(hStatus,		HWND_TOP, 0, height-16, width, height, NULL);

			InvalidateRect(hNewTab, NULL, true);
			InvalidateRect(hButtonCalc, NULL, true);
			InvalidateRect(hResult, NULL, true);
			InvalidateRect(hJITEnabled, NULL, true);
			InvalidateRect(hStatus, NULL, true);
			InvalidateRect(hVars, NULL, true);
		}
			break;
		}
	}__except(EXCEPTION_EXECUTE_HANDLER){
		assert(!"Exception in window procedure handler");
	}
	return DefWindowProc(hWnd, message, wParam, lParam);
}