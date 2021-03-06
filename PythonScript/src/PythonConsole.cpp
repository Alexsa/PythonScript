#include "stdafx.h"
#include "WcharMbcsConverter.h"
#include "PythonConsole.h"
#include "ConsoleDialog.h"
#include "PythonHandler.h"
#include "ProcessExecute.h"
#include "PluginInterface.h"
#include "ScintillaWrapper.h"
#include "PythonScript/NppPythonScript.h"
#include "scintilla.h"

// Sad, but we need to know if we're in an event handler when running an external command
// Not sure how I can extrapolate this info and not tie PythonConsole and NotepadPlusWrapper together.
#include "NotepadPlusWrapper.h"

PythonConsole::PythonConsole(HWND hNotepad) :
	mp_scintillaWrapper(new ScintillaWrapper(NULL)),
	mp_consoleDlg(new ConsoleDialog()),
	mp_mainThreadState(NULL),
	m_statementRunning(CreateEvent(NULL, FALSE, TRUE, NULL)),
	m_hNotepad(hNotepad),
	m_consumerStarted(false),
	m_nppData(new NppData)
{
}

//lint -e1554  Direct pointer copy of member 'name' within copy constructor: 'PythonConsole::PythonConsole(const PythonConsole &)')
// We indeed copy pointers, and it's okay. These are not allocated within the 
// scope of this class but rather passed in and copied anyway.
PythonConsole::PythonConsole(const PythonConsole& other) :
	NppPythonScript::PyProducerConsumer<std::string>(other),
	ConsoleInterface(other),
	mp_scintillaWrapper(other.mp_scintillaWrapper ? new ScintillaWrapper(*other.mp_scintillaWrapper) : NULL),
	mp_consoleDlg(other.mp_consoleDlg ? new ConsoleDialog(*other.mp_consoleDlg) : NULL),
	m_console(other.m_console),
	m_pushFunc(other.m_pushFunc),
	m_sys(other.m_sys),
	mp_mainThreadState(other.mp_mainThreadState),
	m_statementRunning(CreateEvent(NULL, FALSE, TRUE, NULL)),
	m_hNotepad(other.m_hNotepad),
	m_consumerStarted(other.m_consumerStarted),
	m_nppData(other.m_nppData ? new NppData(*other.m_nppData) : NULL)
{
}
//lint +e1554

PythonConsole::~PythonConsole()
{
	try
	{
		delete mp_consoleDlg;
		delete m_nppData;
		delete mp_scintillaWrapper;
	}
	catch (...)
	{
		// I don't know what to do with that, but a destructor should never throw, so...
	}

	if (m_statementRunning)
	{
		CloseHandle(m_statementRunning);
		m_statementRunning = NULL;
	}

	// To please Lint, let's NULL these handles and pointers
	mp_mainThreadState = NULL;
	m_hNotepad = NULL;
}

void PythonConsole::init(HINSTANCE hInst, NppData& nppData)
{
	assert(mp_consoleDlg);
	assert(m_nppData);
	assert(mp_scintillaWrapper);
	if (mp_consoleDlg && m_nppData && mp_scintillaWrapper)
	{
		mp_consoleDlg->initDialog(hInst, nppData, this);
		*m_nppData = nppData;
		mp_scintillaWrapper->setHandle(mp_consoleDlg->getScintillaHwnd());
	}
}

void PythonConsole::initPython(PythonHandler *pythonHandler)
{
	try
	{
		mp_mainThreadState = pythonHandler->getMainThreadState();
		
		PyGILState_STATE gstate = PyGILState_Ensure();

		boost::python::object main_module(boost::python::handle<>(boost::python::borrowed(PyImport_AddModule("__main__"))));
		boost::python::object main_namespace = main_module.attr("__dict__");
		
		// import code
		boost::python::object code = boost::python::import("code");
		main_namespace["code"] = code;

		// import __main__
		main_namespace["__main__"] = main_namespace;
		
		// get ref to code.InteractiveConsole().push()
		m_console = eval("code.InteractiveConsole(__main__)", main_namespace, main_namespace);
		m_pushFunc = m_console.attr("push");

		m_sys = main_namespace["sys"];
	
		PyGILState_Release(gstate);
		
	} 
	catch(...)
	{
		PyErr_Print();
	}
}

void PythonConsole::pythonShowDialog()
{
	assert(mp_consoleDlg);
	if (mp_consoleDlg)
	{
		// Post the message to ourselves (on the right thread) to create the window
		if (!mp_consoleDlg->isCreated())
		{
			CommunicationInfo commInfo;
			commInfo.internalMsg = PYSCR_SHOWCONSOLE;
			commInfo.srcModuleName = _T("PythonScript.dll");
			TCHAR pluginName[] = _T("PythonScript.dll");
			::SendMessage(m_hNotepad, NPPM_MSGTOPLUGIN, reinterpret_cast<WPARAM>(pluginName), reinterpret_cast<LPARAM>(&commInfo));
		}
		else
		{
			mp_consoleDlg->doDialog();
		}
	}
}

void PythonConsole::showDialog()
{
	assert(mp_consoleDlg);
	if (mp_consoleDlg)
	{
		mp_consoleDlg->doDialog();
	}
}

void PythonConsole::hideDialog()
{
	assert(mp_consoleDlg);
	if (mp_consoleDlg)
	{
		mp_consoleDlg->hide();
	}
}

void PythonConsole::message(const char *msg)
{
	assert(mp_consoleDlg);
	if (mp_consoleDlg)
	{
		mp_consoleDlg->writeText(strlen(msg), msg);	
	}
}

void PythonConsole::clear()
{
	assert(mp_consoleDlg);
	if (mp_consoleDlg)
	{
		mp_consoleDlg->clearText();
	}
}


/** To call this function, you MUST have the GIL
 *  (it runs the __str__ attribute of the object)
 *  If you don't, or aren't sure, you can call message() instead, which takes a const char*
 */
void PythonConsole::writeText(boost::python::object text)
{
	assert(mp_consoleDlg);
	if (mp_consoleDlg)
	{
		mp_consoleDlg->writeText(_len(text), (const char *)boost::python::extract<const char *>(text.attr("__str__")()));
	}
}

void PythonConsole::writeError(boost::python::object text)
{
	assert(mp_consoleDlg);
	if (mp_consoleDlg)
	{
		mp_consoleDlg->writeError(_len(text), (const char *)boost::python::extract<const char *>(text.attr("__str__")()));
	}
}

void PythonConsole::stopStatement()
{
	DWORD threadID;
	CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)PythonConsole::stopStatementWorker, this, 0, &threadID);
	
}

DWORD PythonConsole::runCommand(boost::python::str text, boost::python::object pyStdout, boost::python::object pyStderr)
{
	ProcessExecute process;
	std::shared_ptr<TCHAR> cmdLine = WcharMbcsConverter::char2tchar(boost::python::extract<const char *>(text));
	return process.execute(cmdLine.get(), pyStdout, pyStderr, boost::python::object(), NotepadPlusWrapper::isInEvent());
}

void PythonConsole::runStatement(const char *statement)
{
	assert(mp_consoleDlg);
	if (mp_consoleDlg)
	{
		mp_consoleDlg->runEnabled(false);
	}

	// Console statements executed whilst a script is in progress MUST run on a separate 
	// thread.  Otherwise, we wait for the GIL, no problem, except that that blocks the UI thread
	// so if the script happens to be sending a message to scintilla (likely), then 
	// it will deadlock.
	// PyProducerConsumer used here to keep one thread running the actual statements

	if (!m_consumerStarted)
	{
		m_consumerStarted = true;
		startConsumer();
	}

	produce(std::shared_ptr<std::string>(new std::string(statement)));
}

void PythonConsole::queueComplete()
{
	assert(mp_consoleDlg);
	if (mp_consoleDlg)
	{
		mp_consoleDlg->runEnabled(true);
	}
}

void PythonConsole::consume(const std::shared_ptr<std::string>& statement)
{
	PyGILState_STATE gstate = PyGILState_Ensure();
	//const char *prompt = NULL;
	bool continuePrompt = false;
	try
	{
		boost::python::object oldStdout = m_sys.attr("stdout");
		m_sys.attr("stdout") = boost::python::ptr(this);
		boost::python::object result = m_pushFunc(boost::python::str(statement->c_str()));
		m_sys.attr("stdout") = oldStdout;
	
		continuePrompt = boost::python::extract<bool>(result);
		//prompt = extract<const char *>(continuePrompt ? m_sys.attr("ps2") : m_sys.attr("ps1"));
	}
	catch(...)
	{
		PyErr_Print();
	}

	PyGILState_Release(gstate);
	assert(mp_consoleDlg);
	if (mp_consoleDlg)
	{
		mp_consoleDlg->setPrompt(continuePrompt ? "... " : ">>> ");
	}
}

void PythonConsole::stopStatementWorker(PythonConsole *console)
{
	PyGILState_STATE gstate = PyGILState_Ensure();
	
	PyThreadState_SetAsyncExc((long)console->getConsumerThreadID(), PyExc_KeyboardInterrupt);
	
	PyGILState_Release(gstate);
}

void export_console()
{
	//lint -e1793 While calling �Symbol�: Initializing the implicit object parameter �Type� (a non-const reference) with a non-lvalue
	// The class and enum declarations are used as designed, but they mess up Lint.
	boost::python::class_<PythonConsole>("Console", boost::python::no_init)
		.def("write", &PythonConsole::writeText, "Writes text to the console.  Uses the __str__ function of the object passed.")
		.def("clear", &PythonConsole::clear, "Clears the console window")
		.def("writeError", &PythonConsole::writeError, "Writes text in the console in a red colour")
		.def("show", &PythonConsole::pythonShowDialog, "Shows the console")
		.def("hide", &PythonConsole::hideDialog, "Hides the console")
		.def("run", &PythonConsole::runCommand, "Runs a command on the console")
		.def("run", &PythonConsole::runCommandNoStderr, "Runs a command on the console")
		.def("run", &PythonConsole::runCommandNoStdout, "Runs a command on the console")
		.def_readonly("editor", &PythonConsole::mp_scintillaWrapper, "Gets an Editor object for the console window");
	//lint +e1793
}

void PythonConsole::openFile(const char *filename, idx_t lineNo)
{
	std::shared_ptr<TCHAR> tFilename = WcharMbcsConverter::char2tchar(filename);
	if (!SendMessage(m_hNotepad, NPPM_SWITCHTOFILE, 0, reinterpret_cast<LPARAM>(tFilename.get())))
	{
		SendMessage(m_hNotepad, NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(tFilename.get()));
	}

	if (lineNo != IDX_MAX)
	{
		int currentView;
		SendMessage(m_hNotepad, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&currentView));

		assert(m_nppData);
		if (m_nppData)
		{
			SendMessage(currentView ? m_nppData->_scintillaSecondHandle : m_nppData->_scintillaMainHandle, SCI_GOTOLINE, lineNo, 0);
		}
	}
}

HWND PythonConsole::getScintillaHwnd()
{
	assert(mp_consoleDlg);
	if (mp_consoleDlg)
	{
		return mp_consoleDlg->getScintillaHwnd();
	}
	return NULL;
}
