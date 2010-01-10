#include "../NULLC/nullc.h"

#define BUILD_FOR_WINDOWS

#pragma warning(disable : 4996)

#include <stdio.h>
#include <conio.h>
#include <string.h>
#include <time.h>

// NULLC modules
#include "../Modules/includes/file.h"
#include "../Modules/includes/math.h"
#include "../Modules/includes/string.h"

#include "../Modules/includes/canvas.h"

#ifdef BUILD_FOR_WINDOWS
	#include "../Modules/includes/window.h"
	#include "../Modules/includes/io.h"
#endif

int main(int argc, char** argv)
{
	nullcInit();
	nullcSetImportPath("Modules\\");

	nullcAddExternalFunction((void (*)())clock, "int clock();");
	
	nullcInitFileModule();
	nullcInitMathModule();
	nullcInitStringModule();

	nullcInitCanvasModule();

#ifdef BUILD_FOR_WINDOWS
	nullcInitIOModule();
	nullcInitWindowModule();
#endif

	if(argc == 1)
	{
		printf("usage: ConsoleCalc [-x86] file");
		return 0;
	}
	bool useX86 = false;
	const char *fileName = NULL;
	for(int i = 1; i < argc; i++)
	{
		if(strcmp(argv[i], "-x86") == 0)
			useX86 = true;
		if(strstr(argv[i], ".nc"))
			fileName = argv[i];
	}
	if(!fileName)
	{
		printf("File must be specified");
		return 0;
	}

	nullcSetExecutor(useX86 ? NULLC_X86 : NULLC_VM);
	
	FILE *ncFile = fopen(fileName, "rb");
	if(!ncFile)
	{
		printf("File not found");
		return 0;
	}
	fseek(ncFile, 0, SEEK_END);
	unsigned int textSize = ftell(ncFile);
	fseek(ncFile, 0, SEEK_SET);
	char *fileContent = new char[textSize+1];
	fread(fileContent, 1, textSize, ncFile);
	fileContent[textSize] = 0;

	char *bytecode = NULL;

	nullres good = nullcCompile(fileContent);
	if(!good)
	{
		printf("Compilation failed: %s\r\n", nullcGetCompilationError());
	}else{
		nullcGetBytecode(&bytecode);

		nullcClean();
		nullcLinkCode(bytecode, 0);

		nullres goodRun = nullcRun();
		if(goodRun)
		{
			const char* val = nullcGetResult();
			printf("\r\n%s\r\n", val);
		}else{
			printf("Execution failed: %s\r\n", nullcGetRuntimeError());
		}
		delete[] bytecode;
	}

	delete[] fileContent;

	nullcDeinit();
}