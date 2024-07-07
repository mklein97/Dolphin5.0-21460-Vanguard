// A basic test implementation of Netcore for IPC in Dolphin

#pragma warning(disable : 4564)
#pragma warning(disable : 4538)

#include "stdafx.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/State.h"
#include "Core/System.h"

#include <string>

#include "DolphinMemoryDomain.h"
#include "DolphinQT/MainWindow.h"
#include "Helpers.hpp"
#include "VanguardClient.h"
#include "VanguardClientInitializer.h"
#include "VanguardConfigLoader.h"
#include "VanguardSettingsWrapper.h"

#include <msclr/marshal_cppstd.h>
#include <QtGui/QGuiApplication>

ref class VanguardSettingsWrapper;
using namespace cli;
using namespace System;
using namespace Text;
using namespace RTCV;
using namespace RTCV::CorruptCore::Extensions;
using namespace NetCore;
using namespace CorruptCore;
using namespace Vanguard;
using namespace Runtime::InteropServices;
using namespace Threading;
using namespace Collections::Generic;
using namespace Reflection;
using namespace Diagnostics;

#using <system.dll>
#using <system.windows.forms.dll>

#define SRAM_SIZE 25165824
#define ARAM_SIZE 16777216
#define EXRAM_SIZE 67108864

static void EmuThreadExecute(Action ^ callback);
static void EmuThreadExecute(IntPtr ptr);

auto& system = Core::System::GetInstance();

// Define this in here as it's managed and weird stuff happens if it's in a header
public ref class VanguardClient
{
public:
  static NetCoreReceiver ^ receiver;
  static VanguardConnector ^ connector;

  static void OnMessageReceived(Object ^ sender, NetCoreEventArgs ^ e);
  static void SpecUpdated(Object ^ sender, SpecUpdateEventArgs ^ e);
  static void RegisterVanguardSpec();

  static void StartClient();
  static void RestartClient();
  static void StopClient();

  static String ^ RomToLoad;

  static void LoadRom(String ^ filename);
  static bool LoadState(std::string filename);
  static bool SaveState(String ^ filename, bool wait);
  static void CloseGame();

  static String ^ GetConfigAsJson(VanguardSettingsWrapper ^ settings);
  static VanguardSettingsWrapper ^ GetConfigFromJson(String ^ json);

  static String ^ emuDir = IO::Path::GetDirectoryName(Assembly::GetExecutingAssembly()->Location);
  static String ^ logPath = IO::Path::Combine(emuDir, "EMU_LOG.txt");

  static array<String ^> ^ configPaths;

  static volatile bool loading = false;
  static bool attached = false;
  static bool enableRTC = true;
};

static void EmuThreadExecute(Action ^ callback)
{
  EmuThreadExecute(Marshal::GetFunctionPointerForDelegate(callback));
}

static void EmuThreadExecute(IntPtr callbackPtr)
{
  std::function<void(void)> nativeCallback = static_cast<void(__stdcall*)(void)>(callbackPtr.ToPointer());
  Core::RunOnCPUThread(system, nativeCallback, false);
}

static PartialSpec ^ getDefaultPartial() 
{
    PartialSpec ^ partial = gcnew PartialSpec("VanguardSpec");
    partial->Set(VSPEC::NAME, "Dolphin");
    partial->Set(VSPEC::SUPPORTS_RENDERING, false);
    partial->Set(VSPEC::SUPPORTS_CONFIG_MANAGEMENT, true);
    partial->Set(VSPEC::SUPPORTS_CONFIG_HANDOFF, true);
    partial->Set(VSPEC::SUPPORTS_KILLSWITCH, true);
    partial->Set(VSPEC::SUPPORTS_REALTIME, true);
    partial->Set(VSPEC::SUPPORTS_SAVESTATES, true);
    partial->Set(VSPEC::SUPPORTS_REFERENCES, true);
    partial->Set(VSPEC::SUPPORTS_MIXED_STOCKPILE, true);
    partial->Set(VSPEC::CONFIG_PATHS, VanguardClient::configPaths);
    partial->Set(VSPEC::SYSTEM, String::Empty);
    partial->Set(VSPEC::GAMENAME, String::Empty);
    partial->Set(VSPEC::SYSTEMPREFIX, String::Empty);
    partial->Set(VSPEC::OPENROMFILENAME, String::Empty);
    partial->Set(VSPEC::OVERRIDE_DEFAULTMAXINTENSITY, 500000);
    partial->Set(VSPEC::SYNCSETTINGS, String::Empty);
    partial->Set(VSPEC::MEMORYDOMAINS_BLACKLISTEDDOMAINS, gcnew array<String ^>{});
    partial->Set(VSPEC::EMUDIR, VanguardClient::emuDir);

    return partial;
}

void VanguardClient::SpecUpdated(Object ^ sender, SpecUpdateEventArgs ^ e)
{
  PartialSpec ^ partial = e->partialSpec;

  LocalNetCoreRouter::Route(Endpoints::CorruptCore,
                            Commands::Remote::PushVanguardSpecUpdate, partial, true);
  LocalNetCoreRouter::Route(Endpoints::UI, Commands::Remote::PushVanguardSpecUpdate,
                            partial, true);
}

void VanguardClient::RegisterVanguardSpec()
{
  PartialSpec ^ emuSpecTemplate = gcnew PartialSpec("VanguardSpec");

  emuSpecTemplate->Insert(getDefaultPartial());

  AllSpec::VanguardSpec = gcnew FullSpec(emuSpecTemplate, true);
  // You have to feed a partial spec as a template

  if (attached)
    VanguardConnector::PushVanguardSpecRef(AllSpec::VanguardSpec);

  LocalNetCoreRouter::Route(Endpoints::CorruptCore, Commands::Remote::PushVanguardSpec, emuSpecTemplate, true);
  LocalNetCoreRouter::Route(Endpoints::UI, Commands::Remote::PushVanguardSpec, emuSpecTemplate, true);
  AllSpec::VanguardSpec->SpecUpdated += gcnew EventHandler<SpecUpdateEventArgs ^>(&VanguardClient::SpecUpdated);
}

// Lifted from Bizhawk
static Assembly ^ CurrentDomain_AssemblyResolve(Object ^ sender, ResolveEventArgs ^ args) 
{
    try
    {
    Trace::WriteLine("Entering AssemblyResolve\n" + args->Name + "\n" +
                        args->RequestingAssembly);
    String ^ requested = args->Name;
    Monitor::Enter(AppDomain::CurrentDomain);
    {
        array<Assembly ^> ^ asms = AppDomain::CurrentDomain->GetAssemblies();
        for (int i = 0; i < asms->Length; i++)
        {
        Assembly ^ a = asms[i];
        if (a->FullName == requested)
        {
            return a;
        }
        }

        AssemblyName ^ n = gcnew AssemblyName(requested);
        // load missing assemblies by trying to find them in the dll directory
        String ^ dllname = n->Name + ".dll";
        String ^ directory = IO::Path::Combine(
            IO::Path::GetDirectoryName(Assembly::GetExecutingAssembly()->Location), "..", "RTCV");
        String ^ fname = IO::Path::Combine(directory, dllname);
        if (!IO::File::Exists(fname))
        {
        Trace::WriteLine(fname + " doesn't exist");
        return nullptr;
        }

        // it is important that we use LoadFile here and not load from a byte array; otherwise
        // mixed (managed/unamanged) assemblies can't load
        Trace::WriteLine("Loading " + fname);
        return Assembly::UnsafeLoadFrom(fname);
    }
    }
    catch (Exception ^ e)
    {
    Trace::WriteLine("Something went really wrong in AssemblyResolve. Send this to the devs\n" +
                        e);
    return nullptr;
    }
    finally
    {
    Monitor::Exit(AppDomain::CurrentDomain);
    }
}


// Create our VanguardClient
void VanguardClientInitializer::Initialize()
{
  // This has to be in its own method where no other dlls are used so the JIT can compile it
  AppDomain::CurrentDomain->AssemblyResolve +=
      gcnew ResolveEventHandler(CurrentDomain_AssemblyResolve);
  
  ConfigureVisualStyles();
  StartVanguardClient();
}

//This ensures things render as we want them.
//There are no issues running this within QT/WXWidgets applications
//This HAS to be its own method for the JIT. If it's merged with StartVanguardClient(), fun exceptions occur
void VanguardClientInitializer::ConfigureVisualStyles()
{
  // this needs to be done before the warnings/errors show up
  System::Windows::Forms::Application::EnableVisualStyles();
  System::Windows::Forms::Application::SetCompatibleTextRenderingDefault(false);
}

// Create our VanguardClient
void VanguardClientInitializer::StartVanguardClient()
{
  System::Windows::Forms::Form ^ dummy = gcnew System::Windows::Forms::Form();
  IntPtr Handle = dummy->Handle;
  SyncObjectSingleton::SyncObject = dummy;

  SyncObjectSingleton::EmuInvokeDelegate =
      gcnew SyncObjectSingleton::ActionDelegate(&EmuThreadExecute);

  // Start everything
  VanguardClient::configPaths = gcnew array<String ^>{
      IO::Path::Combine(VanguardClient::emuDir, "User", "Config", "Dolphin.ini"),
      IO::Path::Combine(VanguardClient::emuDir, "User", "Config", "GFX.ini"),
      IO::Path::Combine(VanguardClient::emuDir, "User", "Config", "UI.ini"),
      IO::Path::Combine(VanguardClient::emuDir, "User", "Config", "WiimoteNew.ini"),
      IO::Path::Combine(VanguardClient::emuDir, "User", "Config", "GCKeyNew.ini"),
      IO::Path::Combine(VanguardClient::emuDir, "User", "Config", "GCPadNew.ini")};

  VanguardClient::StartClient();
  VanguardClient::RegisterVanguardSpec();
  RtcCore::StartEmuSide();

  // Lie if we're in attached
  if (VanguardClient::attached)
    VanguardConnector::ImplyClientConnected();
}

void VanguardClient::StartClient()
{

  RTCV::Common::ConsoleHelper::CreateConsole(nullptr);
  RTCV::Common::Logging::StartLogging(logPath);
  RTCV::Common::ConsoleHelper::HideConsole();
  // Can't use contains
  auto args = Environment::GetCommandLineArgs();
  for (int i = 0; i < args->Length; i++)
  {
    if (args[i] == "-CONSOLE")
    {
      RTCV::Common::ConsoleHelper::ShowConsole();
    }
    if (args[i] == "-ATTACHED")
    {
      attached = true;
    }
    if (args[i] == "-DISABLERTC")
    {
      enableRTC = false;
    }
  }

  receiver = gcnew NetCoreReceiver();
  receiver->Attached = attached;
  receiver->MessageReceived +=
      gcnew EventHandler<NetCoreEventArgs ^>(&VanguardClient::OnMessageReceived);
  connector = gcnew VanguardConnector(receiver);
}

void VanguardClient::RestartClient()
{
  connector->Kill();
  connector = nullptr;
  StartClient();
}

void VanguardClient::StopClient()
{
  connector->Kill();
  connector = nullptr;
}

#pragma region MemoryDomains
static bool isWii()
{
  if (system.IsWii())
    return true;
  return false;
}

static array<MemoryDomainProxy ^> ^GetInterfaces() 
{
      if (String::IsNullOrWhiteSpace(AllSpec::VanguardSpec->Get<String ^>(VSPEC::OPENROMFILENAME)))
        return gcnew array<MemoryDomainProxy ^>(0);

      array<MemoryDomainProxy ^> ^ interfaces = gcnew array<MemoryDomainProxy ^>(2);
      interfaces[0] = (gcnew MemoryDomainProxy(gcnew SRAM));
      if (isWii())
        interfaces[1] = (gcnew MemoryDomainProxy(gcnew EXRAM));
      else
        interfaces[1] = (gcnew MemoryDomainProxy(gcnew ARAM));

      return interfaces;
    }

static bool RefreshDomains(bool updateSpecs = true)
{
  array<MemoryDomainProxy ^> ^ oldInterfaces =
      AllSpec::VanguardSpec->Get<array<MemoryDomainProxy ^> ^>(VSPEC::MEMORYDOMAINS_INTERFACES);
  array<MemoryDomainProxy ^> ^ newInterfaces = GetInterfaces();

  // Bruteforce it since domains can change inconsistently in some configs and we keep code
  // consistent between implementations
  bool domainsChanged = false;
  if (oldInterfaces == nullptr)
    domainsChanged = true;
  else
  {
    domainsChanged = oldInterfaces->Length != newInterfaces->Length;
    for (int i = 0; i < oldInterfaces->Length; i++)
    {
      if (domainsChanged)
        break;
      if (oldInterfaces[i]->Name != newInterfaces[i]->Name)
        domainsChanged = true;
      if (oldInterfaces[i]->Size != newInterfaces[i]->Size)
        domainsChanged = true;
    }
  }

  if (updateSpecs)
  {
    AllSpec::VanguardSpec->Update(VSPEC::MEMORYDOMAINS_INTERFACES, newInterfaces, true, true);
    LocalNetCoreRouter::Route(Endpoints::CorruptCore,
                              Commands::Remote::EventDomainsUpdated, domainsChanged, true);
  }

  return domainsChanged;
}

#pragma endregion

#pragma region Settings
String ^ VanguardClient::GetConfigAsJson(VanguardSettingsWrapper ^ settings)
{
  return JsonHelper::Serialize(settings);
}

VanguardSettingsWrapper ^ VanguardClient::GetConfigFromJson(String ^ str)
{
  return JsonHelper::Deserialize<VanguardSettingsWrapper ^>(str);
}
#pragma endregion
static void STEP_CORRUPT()  // errors trapped by CPU_STEP
{
  if (!VanguardClient::enableRTC)
    return;
  RtcClock::StepCorrupt(true, true);
}

#pragma region Hooks
void VanguardClientUnmanaged::CORE_STEP()
{
  if (!VanguardClient::enableRTC)
    return;
  // Any step hook for corruption
  STEP_CORRUPT();
}

std::string VanguardClientUnmanaged::GAME_TO_LOAD = "";
// This is on the main thread not the emu thread
void VanguardClientUnmanaged::LOAD_GAME_START(std::string romPath)
{
  if (!VanguardClient::enableRTC)
    return;
  StepActions::ClearStepBlastUnits();
  RtcClock::ResetCount();

  if (romPath == "")
  {
    romPath = GAME_TO_LOAD;
    GAME_TO_LOAD = "";
  }

  String ^ gameName = Helpers::utf8StringToSystemString(romPath);
  AllSpec::VanguardSpec->Update(VSPEC::OPENROMFILENAME, gameName, true, true);
}

void VanguardClientUnmanaged::LOAD_GAME_DONE()
{
  if (!VanguardClient::enableRTC)
    return;

  if (AllSpec::UISpec == nullptr)
  {
    VanguardClient::CloseGame();
    System::Windows::Forms::MessageBox::Show(
        "It appears you haven't connected to StandaloneRTC. Please make sure that the "
        "RTC is running and not just Bizhawk.\nIf you have an antivirus, it might be "
        "blocking the RTC from launching.\n\nIf you keep getting this message, poke "
        "the RTC devs for help (Discord is in the launcher).",
        "RTC Not Connected");
    return;
  }

  PartialSpec ^ gameDone = gcnew PartialSpec("VanguardSpec");

  try
  {
    gameDone->Set(VSPEC::SYSTEM, "Dolphin");
    gameDone->Set(VSPEC::SYSTEMPREFIX, "Dolphin");
    gameDone->Set(VSPEC::SYSTEMCORE, isWii() ? "Wii" : "Gamecube");
    gameDone->Set(VSPEC::SYNCSETTINGS, "");
    gameDone->Set(VSPEC::MEMORYDOMAINS_BLACKLISTEDDOMAINS, gcnew array<String ^>{});
    gameDone->Set(VSPEC::CORE_DISKBASED, true);

    String ^ oldGame = AllSpec::VanguardSpec->Get<String ^>(VSPEC::GAMENAME);
    String ^ gameName =
        Helpers::utf8StringToSystemString(SConfig::GetInstance().GetTitleDescription());

    char replaceChar = L'-';
    gameDone->Set(VSPEC::GAMENAME, StringExtensions::MakeSafeFilename(gameName, replaceChar));

    String ^ syncsettings =
        VanguardClient::GetConfigAsJson(VanguardSettings::GetVanguardSettingsFromDolphin());
    gameDone->Set(VSPEC::SYNCSETTINGS, syncsettings);

    AllSpec::VanguardSpec->Update(gameDone, true, false);

    bool domainsChanged = RefreshDomains(true);

    if (oldGame != gameName)
    {
      LocalNetCoreRouter::Route(Endpoints::UI,
                                Commands::Basic::ResetGameProtectionIfRunning, true);
    }

    RtcCore::InvokeLoadGameDone();
  }
  catch (Exception ^ e)
  {
    Trace::WriteLine(e->ToString());
  }
  VanguardClient::loading = false;
}

void VanguardClientUnmanaged::GAME_CLOSED()
{
  if (!VanguardClient::enableRTC)
    return;

  AllSpec::VanguardSpec->Update(VSPEC::OPENROMFILENAME, "", true, true);
  RefreshDomains();

  // We need to do this or else the thread hangs when you try and X down through the game itself
  // rather than the hex editor
  RtcCore::InvokeGameClosed(true);
}

void VanguardClientUnmanaged::EMULATOR_CLOSING()
{
  if (!VanguardClient::enableRTC)
    return;

  // Make sure we call this from the main thread
  VanguardClient::StopClient();
  RtcCore::InvokeGameClosed(true);
}

bool VanguardClientUnmanaged::RTC_OSD_ENABLED()
{
  if (!VanguardClient::enableRTC)
    return true;
  if (RTCV::NetCore::Params::IsParamSet(RTCSPEC::CORE_EMULATOROSDDISABLED))
    return false;
  return true;
}
#pragma endregion

// No fun anonymous classes with closure here
#pragma region Delegates
void StopGame()
{
  VanguardClientInitializer::win->ForceStopVanguard();
}

void PrepShutdown()
{
  VanguardClientInitializer::win->m_exit_requested = true;
}

void AllSpecsSent()
{
}
#pragma endregion

/*ENUMS FOR THE SWITCH STATEMENT*/
enum COMMANDS
{
  SAVESAVESTATE,
  LOADSAVESTATE,
  REMOTE_LOADROM,
  REMOTE_CLOSEGAME,
  REMOTE_DOMAIN_GETDOMAINS,
  REMOTE_KEY_SETSYNCSETTINGS,
  REMOTE_KEY_SETSYSTEMCORE,
  REMOTE_EVENT_EMU_MAINFORM_CLOSE,
  REMOTE_EVENT_EMUSTARTED,
  REMOTE_ISNORMALADVANCE,
  REMOTE_EVENT_CLOSEEMULATOR,
  REMOTE_ALLSPECSSENT,
  UNKNOWN
};

inline COMMANDS CheckCommand(String ^ inString)
{
  if (String::Compare(inString, NetCore::Commands::Basic::LoadSavestate) == 0)
    return LOADSAVESTATE;
  if (String::Compare(inString, NetCore::Commands::Basic::SaveSavestate) == 0)
    return SAVESAVESTATE;
  if (String::Compare(inString, NetCore::Commands::Remote::LoadROM) == 0)
    return REMOTE_LOADROM;
  if (String::Compare(inString, NetCore::Commands::Remote::CloseGame) == 0)
    return REMOTE_CLOSEGAME;
  if (String::Compare(inString, NetCore::Commands::Remote::AllSpecSent) == 0)
    return REMOTE_ALLSPECSSENT;
  if (String::Compare(inString, NetCore::Commands::Remote::DomainGetDomains) == 0)
    return REMOTE_DOMAIN_GETDOMAINS;
  if (String::Compare(inString, NetCore::Commands::Remote::KeySetSystemCore) == 0)
    return REMOTE_KEY_SETSYSTEMCORE;
  if (String::Compare(inString, NetCore::Commands::Remote::KeySetSyncSettings) == 0)
    return REMOTE_KEY_SETSYNCSETTINGS;
  if (String::Compare(inString, NetCore::Commands::Remote::EventEmuMainFormClose) == 0)
    return REMOTE_EVENT_EMU_MAINFORM_CLOSE;
  if (String::Compare(inString, NetCore::Commands::Remote::EventEmuStarted) == 0)
    return REMOTE_EVENT_EMUSTARTED;
  if (String::Compare(inString, NetCore::Commands::Remote::IsNormalAdvance) == 0)
    return REMOTE_ISNORMALADVANCE;
  if (String::Compare(inString, NetCore::Commands::Remote::EventCloseEmulator) == 0)
    return REMOTE_EVENT_CLOSEEMULATOR;
  return UNKNOWN;
}

/* IMPLEMENT YOUR COMMANDS HERE */
void VanguardClient::LoadRom(String ^ filename)
{
  String ^ currentOpenRom = "";
  if (AllSpec::VanguardSpec->Get<String ^>(VSPEC::OPENROMFILENAME) != "")
    currentOpenRom = AllSpec::VanguardSpec->Get<String ^>(VSPEC::OPENROMFILENAME);

  // Game is not running
  if (currentOpenRom != filename)
  {
    // Clear out any old settings
    Config::ClearCurrentVanguardLayer();

    const std::string& path = Helpers::systemStringToUtf8String(filename);
    loading = true;

    SetState(Core::State::Paused);
    VanguardClientInitializer::win->StartGame(path);
    // We have to do it this way to prevent deadlock due to synced calls. It sucks but it's required
    // at the moment
    while (loading)
    {
      Thread::Sleep(20);
      System::Windows::Forms::Application::DoEvents();
    }

    Thread::Sleep(100);  // Give the emu thread a chance to recover
  }
}

bool VanguardClient::LoadState(std::string filename)
{
  StepActions::ClearStepBlastUnits();
  RtcClock::ResetCount();
  State::LoadAs(system, filename);
  return true;
}

bool VanguardClient::SaveState(String ^ filename, bool wait)
{
  if (Core::IsRunningAndStarted())
  {
    const std::string converted_filename = Helpers::systemStringToUtf8String(filename);
    State::SaveAs(system, converted_filename, wait);
    return true;
  }
  return false;
}

void VanguardClient::CloseGame()
{
  SyncObjectSingleton::GenericDelegate ^ g = gcnew SyncObjectSingleton::GenericDelegate(&StopGame);
  SyncObjectSingleton::FormExecute(g);
  Thread::Sleep(500);  // Sometimes it takes a moment despite claiming it's done
}

/* THIS IS WHERE YOU HANDLE ANY RECEIVED MESSAGES */
void VanguardClient::OnMessageReceived(Object ^ sender, NetCoreEventArgs ^ e)
{
  NetCoreMessage ^ message = e->message;
  NetCoreAdvancedMessage ^ advancedMessage;

  if (Helpers::is<NetCoreAdvancedMessage ^>(message))
    advancedMessage = static_cast<NetCoreAdvancedMessage ^>(message);

  switch (CheckCommand(message->Type))
  {
  case REMOTE_ALLSPECSSENT: {
    auto g = gcnew SyncObjectSingleton::GenericDelegate(&AllSpecsSent);
    SyncObjectSingleton::FormExecute(g);
  }
  break;

  case LOADSAVESTATE: {
    array<Object ^> ^ cmd = static_cast<array<Object ^> ^>(advancedMessage->objectValue);
    String ^ path = static_cast<String ^>(cmd[0]);
    std::string converted_path = Helpers::systemStringToUtf8String(path);

    // Clear out any old settings
    Config::ClearCurrentVanguardLayer();

    // Load up the sync settings
    String ^ settingStr = AllSpec::VanguardSpec->Get<String ^>(VSPEC::SYNCSETTINGS);
    if (!String::IsNullOrWhiteSpace(settingStr))
    {
      VanguardSettingsWrapper ^ settings = GetConfigFromJson(settingStr);
      if (settings != nullptr)
      {
        auto _settings = VanguardSettings::GetVanguardSettingFromVanguardSettingsWrapper(settings);
        AddLayer(ConfigLoaders::GenerateVanguardConfigLoader(&_settings));
      }
    }
    e->setReturnValue(LoadState(converted_path));
  }
  break;

  case SAVESAVESTATE: {
    String ^ Key = (String ^)(advancedMessage->objectValue);

    // Build the shortname
    String ^ quickSlotName = Key + ".timejump";
    // Get the prefix for the state
    String ^ gameName =
        Helpers::utf8StringToSystemString(SConfig::GetInstance().GetTitleDescription());

    char replaceChar = L'-';
    String ^ prefix = StringExtensions::MakeSafeFilename(gameName, replaceChar);
    prefix = prefix->Substring(prefix->LastIndexOf('\\') + 1);

    String ^ path = nullptr;
    // Build up our path
    path = RtcCore::workingDir + IO::Path::DirectorySeparatorChar + "SESSION" +
           IO::Path::DirectorySeparatorChar + prefix + "." + quickSlotName + ".State";

    // If the path doesn't exist, make it
    IO::FileInfo ^ file = gcnew IO::FileInfo(path);
    if (file->Directory != nullptr && file->Directory->Exists == false)
      file->Directory->Create();

    if (SaveState(path, false) && Core::IsRunningAndStarted())
      e->setReturnValue(path);
  }
  break;

  case REMOTE_LOADROM: {
    String ^ filename = (String ^) advancedMessage->objectValue;
    // Dolphin DEMANDS the rom is loaded from the main thread
    Action<String ^> ^ a = gcnew Action<String ^>(&LoadRom);
    SyncObjectSingleton::FormExecute<String ^>(a, filename);
  }
  break;

  case REMOTE_CLOSEGAME: {
    VanguardClient::CloseGame();
  }
  break;

  case REMOTE_DOMAIN_GETDOMAINS: {
    RefreshDomains();
  }
  break;

  case REMOTE_KEY_SETSYNCSETTINGS: {
    String ^ settings = (String ^)(advancedMessage->objectValue);
    AllSpec::VanguardSpec->Set(VSPEC::SYNCSETTINGS, settings);
  }
  break;

  case REMOTE_KEY_SETSYSTEMCORE: {
    // Do nothing
  }
  break;

  case REMOTE_EVENT_EMUSTARTED: {
    // Do nothing
  }
  break;

  case REMOTE_ISNORMALADVANCE: {
    // Todo - Dig out fast forward?
    e->setReturnValue(true);
  }
  break;

  case REMOTE_EVENT_EMU_MAINFORM_CLOSE:
  case REMOTE_EVENT_CLOSEEMULATOR: {


    VanguardClient::StopClient();
    RtcCore::InvokeGameClosed(true);

    // Prep dolphin so when the game closes it exits
    auto g = gcnew SyncObjectSingleton::GenericDelegate(&PrepShutdown);
    SyncObjectSingleton::FormExecute(g);

    // Stop the game
    g = gcnew SyncObjectSingleton::GenericDelegate(&StopGame);
    SyncObjectSingleton::FormExecute(g);

    RtcCore::InvokeKillHexEditor();

  }
  break;

  default:
    break;
  }
}
