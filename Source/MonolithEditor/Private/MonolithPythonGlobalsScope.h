#pragma once

#include "CoreMinimal.h"
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
#include "MonolithJsonUtils.h"

/**
 * Removes the top-level names a Python call adds to the editor's shared console globals, then
 * clears builtins._ and runs gc.collect().
 *
 * Literal code runs in those globals whatever the file scope says -- a private scope is only
 * applied when the command is a .py path -- so a script that leaves a UObject in a variable
 * keeps it alive after the call: FPyReferenceCollector reports every object a live wrapper
 * points at, and with it whatever world that object belongs to. builtins._ holds the last value
 * echoed by an execute_statement call the same way.
 *
 * Only names that did not exist before the call are removed. Anything a script stores elsewhere
 * (module attributes, closures, registered callbacks) is not reached.
 */
class FMonolithPythonGlobalsScope
{
public:
	explicit FMonolithPythonGlobalsScope(IPythonScriptPlugin& InPython, bool bInEnabled = true)
		: Python(InPython)
		, bArmed(bInEnabled && Run(TEXT("__monolith_globals_before = set(globals())")))
	{
		if (bInEnabled && !bArmed)
		{
			UE_LOG(LogMonolith, Warning, TEXT("Python globals snapshot failed; names created by this call will be kept."));
		}
	}

	~FMonolithPythonGlobalsScope()
	{
		if (!bArmed)
		{
			return;
		}

		if (!Run(TEXT("for __monolith_name in [__monolith_n for __monolith_n in globals() if __monolith_n not in __monolith_globals_before]:\n")
				TEXT("    globals().pop(__monolith_name, None)\n")
				TEXT("globals().pop('__monolith_name', None)\n")
				TEXT("__import__('builtins').__dict__.pop('_', None)\n")
				TEXT("__import__('gc').collect()\n")))
		{
			UE_LOG(LogMonolith, Warning, TEXT("Python globals cleanup failed; UObjects referenced from console globals may stay reachable."));
		}
	}

	FMonolithPythonGlobalsScope(const FMonolithPythonGlobalsScope&) = delete;
	FMonolithPythonGlobalsScope& operator=(const FMonolithPythonGlobalsScope&) = delete;

private:
	bool Run(const TCHAR* Code) const
	{
		// ExecuteFile runs literal code as Py_file_input: unlike ExecuteStatement it neither
		// echoes values nor rebinds builtins._.
		FPythonCommandEx Command;
		Command.Command = Code;
		Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		Command.Flags = EPythonCommandFlags::Unattended;
		return Python.ExecPythonCommandEx(Command);
	}

	IPythonScriptPlugin& Python;
	const bool bArmed;
};
