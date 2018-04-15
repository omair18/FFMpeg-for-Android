/*
 * msjexhnd.h
 *
 *  Created on: 2016年1月11日
 *      Author: DELL
 */

#ifndef MSJEXHND_H_
#define MSJEXHND_H_

class MSJExceptionHandler {
public:

	MSJExceptionHandler();
	~MSJExceptionHandler();

	void SetLogFileName(PTSTR pszLogFileName);

private:

	// entry point where control comes on an unhandled exception
	static LONG WINAPI MSJUnhandledExceptionFilter(
			PEXCEPTION_POINTERS pExceptionInfo );

	// where report info is extracted and generated
	static void GenerateExceptionReport(PEXCEPTION_POINTERS pExceptionInfo);

	// Helper functions
	static LPTSTR GetExceptionString(DWORD dwCode);
	static BOOL GetLogicalAddress(PVOID addr, PTSTR szModule, DWORD len,
			DWORD& section, DWORD& offset);
	static void IntelStackWalk(PCONTEXT pContext);
	static int __cdecl _tprintf(const TCHAR * format, ...);

	// Variables used by the class
	static TCHAR m_szLogFileName[MAX_PATH];
	static LPTOP_LEVEL_EXCEPTION_FILTER m_previousFilter;
	static HANDLE m_hReportFile;
};

extern MSJExceptionHandler g_MSJExceptionHandler;   //  global instance of class

#endif /* MSJEXHND_H_ */
