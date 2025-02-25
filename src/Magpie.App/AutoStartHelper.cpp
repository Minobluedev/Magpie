#include "pch.h"
#include "AutoStartHelper.h"
#include <taskschd.h>
#include <Lmcons.h>
#include "Logger.h"
#include "StrUtils.h"
#include "Win32Utils.h"
#include <propkey.h>

#pragma comment(lib, "Taskschd.lib")


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// 实现开机启动
//
// 首先尝试使用任务计划程序，此方案的优点是以管理员身份启动时不会显示 UAC。
// 如果创建任务失败，则回落到在当前用户的启动文件夹中创建快捷方式。
//
// 任务计划程序的使用参考自
// https://github.com/microsoft/PowerToys/blob/3d54cb838504c12f59516afaf1a00fde2dd5d01b/src/runner/auto_start_helper.cpp
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


namespace winrt::Magpie::App {

static constexpr const DWORD USERNAME_DOMAIN_LEN = DNLEN + UNLEN + 2; // Domain Name + '\' + User Name + '\0'
static constexpr const DWORD USERNAME_LEN = UNLEN + 1; // User Name + '\0'


static std::wstring GetTaskName(std::wstring_view userName) {
	return StrUtils::ConcatW(L"Autorun for ", userName);
}

static com_ptr<ITaskService> CreateTaskService() {
	com_ptr<ITaskService> taskService = try_create_instance<ITaskService>(CLSID_TaskScheduler);
	if (!taskService) {
		Logger::Get().Error("创建 TaskService 失败");
		return nullptr;
	}

	HRESULT hr = taskService->Connect(Win32Utils::Variant(), Win32Utils::Variant(),
		Win32Utils::Variant(), Win32Utils::Variant());
	if (FAILED(hr)) {
		Logger::Get().ComError("ITaskService::Connect 失败", hr);
		return nullptr;
	}

	return taskService;
}

static bool CreateAutoStartTask(bool runElevated, const wchar_t* arguments) {
	WCHAR usernameDomain[USERNAME_DOMAIN_LEN];
	WCHAR username[USERNAME_LEN];

	// 检索用户域和用户名
	if (!GetEnvironmentVariable(L"USERNAME", username, USERNAME_LEN)) {
		Logger::Get().Win32Error("获取用户名失败");
		return false;
	}
	if (!GetEnvironmentVariable(L"USERDOMAIN", usernameDomain, USERNAME_DOMAIN_LEN)) {
		Logger::Get().Win32Error("获取用户域失败");
		return false;
	}

	wcscat_s(usernameDomain, L"\\");
	wcscat_s(usernameDomain, username);

	com_ptr<ITaskService> taskService = CreateTaskService();
	if (!taskService) {
		return false;
	}

	// 获取/创建 Magpie 文件夹
	com_ptr<ITaskFolder> taskFolder;
	HRESULT hr = taskService->GetFolder(Win32Utils::BStr(L"\\Magpie"), taskFolder.put());
	if (FAILED(hr)) {
		com_ptr<ITaskFolder> rootFolder = NULL;
		hr = taskService->GetFolder(Win32Utils::BStr(L"\\"), rootFolder.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("获取根目录失败", hr);
			return false;
		}

		hr = rootFolder->CreateFolder(Win32Utils::BStr(L"\\Magpie"), Win32Utils::Variant(L""), taskFolder.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("创建 Magpie 任务文件夹失败", hr);
			return false;
		}
	}

	// Create the task builder object to create the task.
	com_ptr<ITaskDefinition> task;
	hr = taskService->NewTask(0, task.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("创建 ITaskDefinition 失败", hr);
		return false;
	}

	// ------------------------------------------------------
	// Get the registration info for setting the identification.
	com_ptr<IRegistrationInfo> regInfo;
	hr = task->get_RegistrationInfo(regInfo.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取 IRegistrationInfo 失败", hr);
		return false;
	}

	hr = regInfo->put_Author(Win32Utils::BStr(usernameDomain));
	if (FAILED(hr)) {
		Logger::Get().ComError("IRegistrationInfo::put_Author 失败", hr);
		return false;
	}

	 // ------------------------------------------------------
	// Create the settings for the task
	com_ptr<ITaskSettings> taskSettings;
	hr = task->get_Settings(taskSettings.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取 ITaskSettings 失败", hr);
		return false;
	}

	hr = taskSettings->put_StartWhenAvailable(VARIANT_FALSE);
	if (FAILED(hr)) {
		Logger::Get().ComError("ITaskSettings::put_StartWhenAvailable 失败", hr);
		return false;
	}
	hr = taskSettings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
	if (FAILED(hr)) {
		Logger::Get().ComError("ITaskSettings::put_StopIfGoingOnBatteries 失败", hr);
		return false;
	}
	hr = taskSettings->put_ExecutionTimeLimit(Win32Utils::BStr(L"PT0S")); //Unlimited
	if (FAILED(hr)) {
		Logger::Get().ComError("ITaskSettings::put_ExecutionTimeLimit 失败", hr);
		return false;
	}
	hr = taskSettings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
	if (FAILED(hr)) {
		Logger::Get().ComError("ITaskSettings::put_DisallowStartIfOnBatteries 失败", hr);
		return false;
	}

	// ------------------------------------------------------
	// Get the trigger collection to insert the logon trigger.
	com_ptr<ITriggerCollection> triggerCollection;
	hr = task->get_Triggers(triggerCollection.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取 ITriggerCollection 失败", hr);
		return false;
	}

	// Add the logon trigger to the task.
	{
		com_ptr<ITrigger> trigger;
		hr = triggerCollection->Create(TASK_TRIGGER_LOGON, trigger.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("创建 ITrigger 失败", hr);
			return false;
		}

		com_ptr<ILogonTrigger> logonTrigger = trigger.try_as<ILogonTrigger>();
		if (!logonTrigger) {
			Logger::Get().Error("获取 ILogonTrigger 失败");
			return false;
		}

		logonTrigger->put_Id(Win32Utils::BStr(L"Trigger1"));

		// Timing issues may make explorer not be started when the task runs.
		// Add a little delay to mitigate this.
		logonTrigger->put_Delay(Win32Utils::BStr(L"PT03S"));

		// Define the user. The task will execute when the user logs on.
		// The specified user must be a user on this computer.
		hr = logonTrigger->put_UserId(Win32Utils::BStr(usernameDomain));
		if (FAILED(hr)) {
			Logger::Get().ComError("ILogonTrigger::put_UserId 失败", hr);
			return false;
		}
	}

	// ------------------------------------------------------
	// Add an Action to the task. This task will execute the path passed to this custom action.
	{
		com_ptr<IActionCollection> actionCollection;

		// Get the task action collection pointer.
		hr = task->get_Actions(actionCollection.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("获取 IActionCollection 失败", hr);
			return false;
		}

		// Create the action, specifying that it is an executable action.
		com_ptr<IAction> action;
		hr = actionCollection->Create(TASK_ACTION_EXEC, action.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("创建 IAction 失败", hr);
			return false;
		}

		// QI for the executable task pointer.
		com_ptr<IExecAction> execAction = action.try_as<IExecAction>();
		if (!execAction) {
			Logger::Get().Error("获取 IExecAction 失败");
			return false;
		}

		// Set the path of the executable to Magpie (passed as CustomActionData).
		WCHAR executablePath[MAX_PATH];
		GetModuleFileName(NULL, executablePath, MAX_PATH);
		hr = execAction->put_Path(Win32Utils::BStr(executablePath));
		if (FAILED(hr)) {
			Logger::Get().ComError("设置可执行文件路径失败", hr);
			return false;
		}

		if (arguments) {
			execAction->put_Arguments(Win32Utils::BStr(arguments));
		}
	}

	// ------------------------------------------------------
	// Create the principal for the task
	{
		com_ptr<IPrincipal> principal;
		hr = task->get_Principal(principal.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("获取 IPrincipal 失败", hr);
			return false;
		}

		// Set up principal information:
		principal->put_Id(Win32Utils::BStr(L"Principal1"));
		principal->put_UserId(Win32Utils::BStr(usernameDomain));
		principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);

		if (runElevated) {
			hr = principal->put_RunLevel(_TASK_RUNLEVEL::TASK_RUNLEVEL_HIGHEST);
		} else {
			hr = principal->put_RunLevel(_TASK_RUNLEVEL::TASK_RUNLEVEL_LUA);
		}

		if (FAILED(hr)) {
			Logger::Get().ComError("IPrincipal::put_RunLevel 失败", hr);
			return false;
		}
	}

	// ------------------------------------------------------
	//  Save the task in the Magpie folder.
	{
		com_ptr<IRegisteredTask> registeredTask;
		static constexpr const wchar_t* SDDL_FULL_ACCESS_FOR_EVERYONE = L"D:(A;;FA;;;WD)";

		// 如果用户是 Administrator 账户，但 Magpie 不是以提升权限运行的，此调用会因权限问题失败
		std::wstring taskName = GetTaskName(username);
		hr = taskFolder->RegisterTaskDefinition(
			Win32Utils::BStr(taskName),
			task.get(),
			TASK_CREATE_OR_UPDATE,
			Win32Utils::Variant(usernameDomain),
			Win32Utils::Variant(),
			TASK_LOGON_INTERACTIVE_TOKEN,
			Win32Utils::Variant(SDDL_FULL_ACCESS_FOR_EVERYONE),
			registeredTask.put()
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("注册任务失败", hr);
			return false;
		}

		registeredTask->put_Enabled(VARIANT_TRUE);
	}

	return true;
}

static bool DeleteAutoStartTask() {
	WCHAR username[USERNAME_LEN];
	if (!GetEnvironmentVariable(L"USERNAME", username, USERNAME_LEN)) {
		Logger::Get().Win32Error("获取用户名失败");
		return false;
	}

	com_ptr<ITaskService> taskService = CreateTaskService();
	if (!taskService) {
		return false;
	}

	com_ptr<ITaskFolder> taskFolder;
	HRESULT hr = taskService->GetFolder(Win32Utils::BStr(L"\\Magpie"), taskFolder.put());
	if (FAILED(hr)) {
		return true;
	}

	Win32Utils::BStr taskName(GetTaskName(username));

	{
		com_ptr<IRegisteredTask> existingRegisteredTask;
		hr = taskFolder->GetTask(taskName, existingRegisteredTask.put());
		if (FAILED(hr)) {
			// 不存在任务
			return true;
		}
	}

	hr = taskFolder->DeleteTask(taskName, 0);
	if (FAILED(hr)) {
		Logger::Get().ComError("删除任务失败", hr);
		return false;
	}

	return true;
}

static bool IsAutoStartTaskActive(std::wstring& arguements) {
	WCHAR username[USERNAME_LEN];
	if (!GetEnvironmentVariable(L"USERNAME", username, USERNAME_LEN)) {
		Logger::Get().Win32Error("获取用户名失败");
		return false;
	}

	com_ptr<ITaskService> taskService = CreateTaskService();
	if (!taskService) {
		return false;
	}

	com_ptr<ITaskFolder> taskFolder;
	HRESULT hr = taskService->GetFolder(Win32Utils::BStr(L"\\Magpie"), taskFolder.put());
	if (FAILED(hr)) {
		return false;
	}

	com_ptr<IRegisteredTask> existingRegisteredTask;
	hr = taskFolder->GetTask(Win32Utils::BStr(GetTaskName(username)), existingRegisteredTask.put());
	if (FAILED(hr)) {
		return false;
	}

	VARIANT_BOOL isEnabled;
	hr = existingRegisteredTask->get_Enabled(&isEnabled);
	if (FAILED(hr)) {
		Logger::Get().ComError("IRegisteredTask::get_Enabled 失败", hr);
		return false;
	}

	com_ptr<ITaskDefinition> task;
	hr = existingRegisteredTask->get_Definition(task.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取 ITaskDefinition 失败", hr);
		return false;
	}

	com_ptr<IActionCollection> actionCollection;
	task->get_Actions(actionCollection.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取 IActionCollection 失败", hr);
		return false;
	}

	com_ptr<IAction> action;
	// 索引从 1 开始
	hr = actionCollection->get_Item(1, action.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取 IAction 失败", hr);
		return false;
	}

	com_ptr<IExecAction> execAction = action.try_as<IExecAction>();
	Win32Utils::BStr args;
	hr = execAction->get_Arguments(&args.Raw());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取参数失败", hr);
		return false;
	}
	arguements = args.ToString();

	return isEnabled == VARIANT_TRUE;
}

static std::wstring GetShortcutPath() {
	std::wstring shortcutPath;

	// 获取用户的启动文件夹路径
	wchar_t* startupDir = nullptr;
	HRESULT hr = SHGetKnownFolderPath(FOLDERID_Startup, 0, NULL, &startupDir);
	if (FAILED(hr)) {
		CoTaskMemFree(startupDir);
		Logger::Get().ComError("获取启动文件夹失败", hr);
		return {};
	}

	shortcutPath = StrUtils::ConcatW(startupDir, L"\\Magpie.lnk");
	CoTaskMemFree(startupDir);

	return shortcutPath;
}

static bool CreateAutoStartShortcut(const wchar_t* arguments) {
	com_ptr<IShellLink> shellLink = try_create_instance<IShellLink>(CLSID_ShellLink);
	if (!shellLink) {
		Logger::Get().Error("创建 ShellLink 失败");
		return false;
	}

	WCHAR executablePath[MAX_PATH];
	GetModuleFileName(NULL, executablePath, MAX_PATH);
	shellLink->SetPath(executablePath);

	if (arguments) {
		shellLink->SetArguments(arguments);
	}

	com_ptr<IPersistFile> persistFile = shellLink.try_as<IPersistFile>();
	if (!persistFile) {
		Logger::Get().Error("获取 IPersistFile 失败");
		return false;
	}

	HRESULT hr = persistFile->Save(GetShortcutPath().c_str(), TRUE);
	if (FAILED(hr)) {
		Logger::Get().ComError("保存快捷方式失败", hr);
		return false;
	}

	return true;
}

static bool DeleteAutoStartShortcut() {
	std::wstring shortcutPath = GetShortcutPath();
	if (shortcutPath.empty()) {
		return false;
	}

	if (!Win32Utils::FileExists(shortcutPath.c_str())) {
		return true;
	}

	if (!DeleteFile(shortcutPath.c_str())) {
		Logger::Get().Win32Error("删除快捷方式失败");
		return false;
	}

	return true;
}

static bool IsAutoStartShortcutExist(std::wstring& arguments) {
	std::wstring shortcutPath = GetShortcutPath();
	if (shortcutPath.empty()) {
		return false;
	}

	if (!Win32Utils::FileExists(shortcutPath.c_str())) {
		return false;
	}

	com_ptr<IShellLink> shellLink = try_create_instance<IShellLink>(CLSID_ShellLink);
	if (!shellLink) {
		Logger::Get().Error("创建 ShellLink 失败");
		return false;
	}

	com_ptr<IPersistFile> persistFile = shellLink.try_as<IPersistFile>();
	if (!persistFile) {
		Logger::Get().Error("获取 IPersistFile 失败");
		return false;
	}

	HRESULT hr = persistFile->Load(shortcutPath.c_str(), STGM_READ);
	if (FAILED(hr)) {
		Logger::Get().ComError("读取快捷方式失败", hr);
		return false;
	}

	hr = shellLink->Resolve(NULL, SLR_NO_UI);
	if (FAILED(hr)) {
		Logger::Get().ComError("解析快捷方式失败", hr);
		return false;
	}

	// https://docs.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishelllinka-getarguments
	// 推荐从 IPropertyStore 检索参数
	com_ptr<IPropertyStore> propertyStore = shellLink.as<IPropertyStore>();
	if (!propertyStore) {
		Logger::Get().Error("获取 IPropertyStore 失败");
		return false;
	}

	PROPVARIANT prop;
	hr = propertyStore->GetValue(PKEY_Link_Arguments, &prop);
	if (FAILED(hr)) {
		Logger::Get().ComError("检索 Arguments 参数失败", hr);
		return false;
	}

	if (prop.vt == VT_LPWSTR) {
		arguments = prop.pwszVal;
	} else if (prop.vt == VT_BSTR) {
		arguments = prop.bstrVal;
	}

	PropVariantClear(&prop);

	return true;
}

bool AutoStartHelper::EnableAutoStart(bool runElevated, const wchar_t* arguments) {
	if (CreateAutoStartTask(runElevated, arguments)) {
		DeleteAutoStartShortcut();
		return true;
	}

	return CreateAutoStartShortcut(arguments);
}

bool AutoStartHelper::DisableAutoStart() {
	bool result1 = DeleteAutoStartTask();
	bool result2 = DeleteAutoStartShortcut();
	return result1 || result2;
}

bool AutoStartHelper::IsAutoStartEnabled(std::wstring& arguments) {
	return IsAutoStartTaskActive(arguments) || IsAutoStartShortcutExist(arguments);
}

}
