#include "missions.hh"
#include <cstdlib>
#include "logger.hh"
#include "base.hh"
#include "functions.hh"
#include <vector>
#include "injector/injector.hpp"
#include "config.hh"
#include "util/scrpt.hh"
#include <algorithm>
#include "injector/calling.hpp"
#include "autosave.hh"
#include "missions_data.hh"
#include <stdexcept>
#include <array>
#include <random>
#include <memory>
#include "util/loader.hh"
#include "dyom.hh"

MissionRandomizer *MissionRandomizer::mInstance = nullptr;

const int START_MISSIONS     = 11;
const int END_MISSIONS       = 112;
const int UNLOCKED_CITY_STAT = 181;

int exceptions[] = {
    40, // First Date
    35, // Race Tournament / 8-track / Dirt Track
    83, // Learning to Fly
    71, // Back To School
};

/*******************************************************/
void
Teleport (Position pos, bool refresh = true)
{
    if (refresh)
        {
            Scrpt::CallOpcode (0x4BB, "select_interior", 0);
            Scrpt::CallOpcode (0x4FA, "reset_sky_colours_with_fade", 0);
            Scrpt::CallOpcode (0x57E, "set_radar_grey", 0);
            Scrpt::CallOpcode (0x4E4, "refresh_game_renderer", pos.x, pos.y);
            Scrpt::CallOpcode (0x860, "link_actor to_interior", GlobalVar (3),
                               0);
            Scrpt::CallOpcode (0xA0B, "set_rendering_origin", pos.x, pos.y,
                               pos.z, pos.heading);
        }
    CRunningScript::SetCharCoordinates (FindPlayerPed (), {pos.x, pos.y, pos.z},
                                        1, 1);
    FindPlayerEntity ()->SetHeading (pos.heading * 3.1415926 / 180.0);
}

/*******************************************************/
void
RandomizePropertyToBuy ()
{
    int original_property = ScriptSpace[1735];
    // if(origianl_property >
}

/*******************************************************/
void __fastcall RandomizeMissionToStart (CRunningScript *scr, void *edx,
                                         short count)
{
    auto missionRandomizer = MissionRandomizer::GetInstance ();

    scr->CollectParameters (count);
    if (ScriptParams[0] >= START_MISSIONS && ScriptParams[0] <= END_MISSIONS)
        {
            if (missionRandomizer->mContinuedMission != ScriptParams[0])
                {
                    missionRandomizer->mOriginalMissionNumber = ScriptParams[0];
                }
            else
                missionRandomizer->SetContinuedMission (-1);

            ScriptParams[0]
                = missionRandomizer->GetRandomMission (ScriptParams[0]);

            missionRandomizer->mRandomizedMissionNumber = ScriptParams[0];
            if (ScriptParams[0] != missionRandomizer->mOriginalMissionNumber)
                missionRandomizer->TeleportPlayerBeforeMission ();

            if (threadFinishes.count (ScriptParams[0]))
                missionRandomizer->mStoreNextMission = true;
        }

    if (ScriptParams[0] == 134) // Buy Properties Mission
        RandomizePropertyToBuy ();
}

/*******************************************************/
void
MissionRandomizer::TeleportPlayerBeforeMission ()
{
    if (missionStartPos.count (mRandomizedMissionNumber))
        Teleport (missionStartPos[mRandomizedMissionNumber]);
}

/*******************************************************/
void
MissionRandomizer::TeleportPlayerAfterMission ()
{
    try
        {
            int status   = GetStatusForTwoPartMissions (mOriginalMissionNumber);
            Position pos = missionEndPos.at (mOriginalMissionNumber)[status];
            pos.z        = (mRandomizedMissionNumber == 80) ? 1500 : pos.z;

            Teleport (pos);
        }
    catch (const std::out_of_range &e)
        {
        }
}

/*******************************************************/
int
MissionRandomizer::GetStatusForTwoPartMissions (int index)
{
    switch (index)
        {
        // House Party
        case 34: return ScriptSpace[455] > 3;

        // Wu Zi Mu and Farewell My Love
        case 48: return ScriptSpace[492] > 4;

        // Jizzy
        case 59: return ScriptSpace[545] > 1;

        default: return 0;
        }
}

/*******************************************************/
int
MissionRandomizer::GetRandomMission (int originalMission)
{
    for (auto i : exceptions)
        if (originalMission == i)
            return originalMission;

    // Forced Mission
    auto config = ConfigManager::GetInstance ()->GetConfigs ().missions;
    if (config.forcedMissionEnabled)
        return config.forcedMissionID;

    if (config.shufflingEnabled)
        {
            if (mShuffledOrder.count (originalMission))
                {
                    int index = GetStatusForTwoPartMissions (originalMission);
                    if (mShuffledOrder[originalMission].size () <= index)
                        index = 0;

                    return mShuffledOrder[originalMission][index];
                }
        }

    std::vector<uint8_t> missionList;
    for (int i = START_MISSIONS; i < END_MISSIONS; i++)
        {
            if (std::find (std::begin (exceptions), std::end (exceptions), i)
                != std::end (exceptions))
                continue;

            if (mSaveInfo.missionStatus[i])
                missionList.push_back (i);
        }

    if (missionList.size () == 0)
        return originalMission;

    return missionList[random (missionList.size () - 1)];
}

/*******************************************************/
void
MissionRandomizer::HandleReturnOpcode (CRunningScript *scr, short opcode)
{
    const int OPCODE_RETURN = 81;
    if (opcode != OPCODE_RETURN || !mScriptReplaced)
        return;

    // Restore original base ip
    scr->m_pBaseIP = mOriginalBaseIP;

    // Restore local variables
    memcpy ((int *) 0xA48960, this->mLocalVariables, 0x400 * sizeof (uint32_t));

    mScriptReplaced         = false;
    this->mRandomizedScript = nullptr;
}

/*******************************************************/
void
MissionRandomizer::HandleGoSubOpcode (CRunningScript *scr, short &opcode)
{
    const int OPCODE_GOSUB = 0x50;
    if (opcode != OPCODE_GOSUB || !mScriptReplaced)
        return;

    scr->m_pCurrentIP += 2;
    mRandomizedScript->CollectParameters (1); // skip
    HandleGoSubAlternativeForMission (mOriginalMissionNumber);

    opcode = *reinterpret_cast<uint16_t *> (scr->m_pCurrentIP);
}

/*******************************************************/
void
MissionRandomizer::HandleStoreCarOpcode (CRunningScript *scr, short opcode)
{
    const int OPCODE_STORE_CAR_CHAR_IS_IN = 0xD9;
    if (opcode != OPCODE_STORE_CAR_CHAR_IS_IN || mRandomizedMissionNumber != 36)
        return;

    // Put player in a random vehicle
    if (!FindPlayerVehicle ())
        {
            constexpr int vehicle = 567;

            CStreaming::RequestModel (vehicle, 0);
            CStreaming::LoadAllRequestedModels (0);
            if (ms_aInfoForModel[vehicle].m_nLoadState == 1)
                {
                    Scrpt::CallOpcode (0xa5, "create_car", 567, 0.0f, 0.0f,
                                       0.0f, GlobalVar (2197));
                    Scrpt::CallOpcode (0x036A, "put_actor_in_car",
                                       GlobalVar (3), ScriptSpace[2197]);
                }
            else
                {
                    Logger::GetLogger ()->LogMessage (
                        "High Stakes failed to successfully "
                        "spawn a random vehicle for the "
                        "player");
                }
        }
}

/*******************************************************/
void
MissionRandomizer::HandleReplaceMissionOpcode (CRunningScript *scr,
                                               short           opcode)
{
    if (opcode != OPCODE_REPLACE_MISSION)
        return;

    mRandomizedScript->m_pCurrentIP += 2;
    mRandomizedScript->CollectParameters (1);

    mRandomizedScript->ProcessCommands0to99 (0x4E);

    Scrpt::CallOpcode (0x417, "start_mission", ScriptParams[0]);
}

/*******************************************************/
void
MissionRandomizer::HandleEndThreadOpcode (CRunningScript *scr, short opcode)
{
    const int OPCODE_END_THREAD = 78;
    if (opcode != OPCODE_END_THREAD)
        return;

    RestoreCityInfo (this->mCityInfo);
    SetGangTerritoriesForMission (mOriginalMissionNumber);
    SetRiotModeForMission (mOriginalMissionNumber);

    this->ApplyMissionFailFixes ();
    this->mRandomizedScript = nullptr;
}

/*******************************************************/
void
MissionRandomizer::HandleOverrideRestartOpcode (CRunningScript *scr,
                                                short           opcode)
{
    const int OPCODE_OVERRIDE_RESTART = 0x16E;
    if (opcode != OPCODE_OVERRIDE_RESTART)
        return;

    scr->m_pCurrentIP += 2;
    scr->CollectParameters (4);
}

/*******************************************************/
bool
MissionRandomizer::ShouldJump (CRunningScript *scr)
{
    const int OPCODE_END_THREAD           = 78;
    const int OPCODE_RETURN               = 81;
    const int OPCODE_STORE_CAR_CHAR_IS_IN = 0xD9;
    const int OPCODE_GOSUB                = 0x50;

    int currentOffset = scr->m_pCurrentIP - scr->m_pBaseIP;
    if (currentOffset != this->mPrevOffset)
        {
            short opCode = *reinterpret_cast<uint16_t *> (scr->m_pCurrentIP);

            HandleOverrideRestartOpcode (scr, opCode);
            HandleGoSubOpcode (scr, opCode);
            HandleEndThreadOpcode (scr, opCode);
            HandleStoreCarOpcode (scr, opCode);
            HandleReplaceMissionOpcode (scr, opCode);
            HandleReturnOpcode (scr, opCode);

            this->mPrevOffset = scr->m_pCurrentIP - scr->m_pBaseIP;
        }

    if (scr == this->mRandomizedScript)
        {
            if (mScriptByPass)
                {
                    SetScriptByPass (false);
                    return true;
                }

            for (auto i : threadFinishes[this->mRandomizedMissionNumber])
                {
                    if (i == currentOffset)
                        return true;
                }
        }
    return false;
}

/*******************************************************/
void __fastcall UpdatePCHook (CRunningScript *scr, void *edx, int a2)
{
    CallMethod<0x464DA0> (scr, a2);
}

/*******************************************************/
int
MissionRandomizer::GetCorrectedMissionNo ()
{
    return this->mOriginalMissionNumber;
}

/*******************************************************/
void
MissionRandomizer::MoveScriptToOriginalOffset (CRunningScript *scr)
{
    int *missionOffsets = (int *) 0xA444C8;

    int baseOffset = missionOffsets[GetCorrectedMissionNo ()];
    int status     = GetStatusForTwoPartMissions (mOriginalMissionNumber);
    int offset     = threadFinishes[GetCorrectedMissionNo ()][status];

    Logger::GetLogger ()->LogMessage ("Jumping to original script cleanup");

    FILE *scm = fopen (GetGameDirRelativePathA ("data/script/main.scm"), "rb");
    fseek (scm, baseOffset, SEEK_SET);

    mScriptReplaced = true;
    mOriginalBaseIP = scr->m_pBaseIP;

    fread (this->mTempMissionData, 1, 69000, scm);
    scr->m_pBaseIP    = this->mTempMissionData;
    scr->m_pCurrentIP = scr->m_pBaseIP + offset;

    this->TeleportPlayerAfterMission ();
    RestoreCityInfo (this->mCityInfo);
    this->SetGangTerritoriesForMission (this->mOriginalMissionNumber);
    this->SetRiotModeForMission (this->mOriginalMissionNumber);
    this->ApplyMissionSpecificFixes (this->mTempMissionData);
    AutoSave::GetInstance ()->SetShouldSave (true);

    mSaveInfo.missionStatus[GetCorrectedMissionStatusIndex (
        mRandomizedMissionNumber)]--;
    SetCorrectedMissionStatusIndex (-1, -1);

    memcpy (this->mLocalVariables, (int *) 0xA48960, 0x400 * sizeof (uint32_t));
    // needn't reset local variables if we're jumping to the same mission
    if (mRandomizedMissionNumber != mOriginalMissionNumber)
        memset ((int *) 0xA48960, 0, 0x400 * sizeof (uint32_t));

    fclose (scm);
}

/*******************************************************/
int
MissionRandomizer::GetCorrectedMissionStatusIndex (int index)
{
    if (index != this->mCorrectedMissionStatus.first)
        return index;
    return this->mCorrectedMissionStatus.second;
}

/*******************************************************/
void
JumpOnMissionEnd ()
{
    auto missionRandomizer = MissionRandomizer::GetInstance ();
    auto dyomRandomizer    = DyomRandomizer::GetInstance ();

    if (missionRandomizer->mRandomizedScript
        && missionRandomizer->ShouldJump (missionRandomizer->mRandomizedScript))
        missionRandomizer->MoveScriptToOriginalOffset (
            missionRandomizer->mRandomizedScript);

    if (DyomRandomizer::mEnabled && dyomRandomizer->mDyomScript)
        dyomRandomizer->HandleDyomScript (
            dyomRandomizer->mDyomScript);

    HookManager::CallOriginalAndReturn<injector::cstd<void ()>, 0x469FB0> (
        [] { (*((int *) 0xA447F4))++; });
}

/*******************************************************/
bool
IsIPLEnabled (char *name)
{
    int     index = CIplStore::FindIplSlot (name);
    IplDef *def   = CIplStore::ms_pPool->GetAt<IplDef> (index);
    if (!def->field2D || !def->m_bDisableDynamicStreaming)
        return false;

    return true;
}

/*******************************************************/
void
MissionRandomizer::StoreCityInfo (CitiesInfo &out)
{
    out.citiesUnlocked = CStats::GetStatValue (UNLOCKED_CITY_STAT);
    out.LVBarriers     = IsIPLEnabled ((char *) "BARRIERS2");
    out.SFBarriers     = IsIPLEnabled ((char *) "BARRIERS1");
    Scrpt::CallOpcode (0x50F, "get_max_wanted_level", &out.maxWanted);
}

/*******************************************************/
void
MissionRandomizer::RestoreCityInfo (const CitiesInfo &info)
{
    this->mCurrentCitiesUnlocked = info.citiesUnlocked;

    static auto handleBridge = [] (bool bridge, const char *id) {
        if (bridge)
            Scrpt::CallOpcode (0x776, "create_objects", id);
        else
            Scrpt::CallOpcode (0x777, "remove_objects", id);
    };

    handleBridge (info.LVBarriers, "BARRIERS2");
    handleBridge (info.SFBarriers, "BARRIERS1");

    Scrpt::CallOpcode (0x1F0, "set_max_wanted_level", info.maxWanted);
}

/*******************************************************/
void
MissionRandomizer::SetRiotModeForMission (int index)
{
    bool riot = false;
    if (index >= 108 && index <= 112)
        riot = true;

    Scrpt::CallOpcode (0x6C8, "enable_riot", riot ? 1 : 0);
}

/*******************************************************/
void
MissionRandomizer::SetGangTerritoriesForMission (int index)
{
    bool wars = false;

    if ((ScriptSpace[458] < 1 && ScriptSpace[452] >= 8) || index > 104)
        wars = true;

    Scrpt::CallOpcode (0x879, "enable_gang_wars", wars ? 1 : 0);
}

/*******************************************************/
void
MissionRandomizer::UnlockCitiesBasedOnMissionID (int missionId)
{
    static std::array<std::pair<int, CitiesInfo>, 4> cities
        = {{{92, {3, false, false, 6}},
            {63, {2, false, false, 6}},
            {38, {1, false, true, 6}},
            {0, {0, true, true, 6}}}};

    if (!this->mRandomizedScript)
        StoreCityInfo (this->mCityInfo);

    for (auto i : cities)
        {
            if (i.first < missionId)
                {
                    RestoreCityInfo (i.second);
                    return;
                }
        }
}

/*******************************************************/
CRunningScript *
StoreRandomizedScript (uint8_t *startIp)
{
    auto missionRandomizer = MissionRandomizer::GetInstance ();

    auto out = HookManager::CallOriginalAndReturn<
        injector::cstd<CRunningScript *(uint8_t *)>, 0x489A7A> (nullptr,
                                                                startIp);

    if (missionRandomizer->mStoreNextMission)
        {
            missionRandomizer->ApplyMissionStartSpecificFixes (startIp);
            missionRandomizer->UnlockCitiesBasedOnMissionID (
                missionRandomizer->mRandomizedMissionNumber);

            missionRandomizer->SetGangTerritoriesForMission (
                missionRandomizer->mRandomizedMissionNumber);
            missionRandomizer->SetRiotModeForMission (
                missionRandomizer->mRandomizedMissionNumber);
            missionRandomizer->mRandomizedScript = out;
        }

    missionRandomizer->mStoreNextMission = false;
    return out;
}

/*******************************************************/
float
UnlockCities (int statsId)
{
    auto missionRandomizer = MissionRandomizer::GetInstance ();
    if (missionRandomizer->mRandomizedScript)
        return missionRandomizer->mCurrentCitiesUnlocked;

    return HookManager::CallOriginalAndReturn<injector::cstd<float (int)>,
                                              0x4417F5> (3, 181);
}

/*******************************************************/
int
CorrectMaxNumberOfGroupMembers ()
{
    auto missionRandomizer = MissionRandomizer::GetInstance ();

    int max
        = HookManager::CallOriginalAndReturn<injector::cstd<int ()>, 0x60C925> (
            3);

    if (missionRandomizer->mRandomizedScript
        && missionRandomizer->mRandomizedMissionNumber == 109)
        return std::max (3, max);

    return max;
}

/*******************************************************/
void
SaveMissionData ()
{
    // CTheScripts::Save
    HookManager::CallOriginal<injector::cstd<void ()>, 0x5D15A6> ();
    MissionRandomizer::GetInstance ()->Save ();
}

/*******************************************************/
void
LoadMissionData ()
{
    HookManager::CallOriginal<injector::cstd<void ()>, 0x5D19CE> ();
    MissionRandomizer::GetInstance ()->Load ();
}

/*******************************************************/
void
InitAtNewGame ()
{
    HookManager::CallOriginal<injector::cstd<void ()>, 0x53BE76> ();
    MissionRandomizer::GetInstance ()->ResetSaveData ();
    MissionRandomizer::GetInstance ()->InitShuffledMissionOrder ();
}

/*******************************************************/
void
MissionRandomizer::ResetSaveData ()
{
    auto config = ConfigManager::GetInstance ()->GetConfigs ().missions;
    this->mRandomizedScript = nullptr;

    mSaveInfo.randomSeed = config.shufflingSeed;
    if (config.shufflingEnabled && config.shufflingSeed == -1)
        mSaveInfo.randomSeed = random (UINT_MAX);

    memset (mSaveInfo.missionStatus.data, 1,
            sizeof (mSaveInfo.missionStatus.data));

    mSaveInfo.missionStatus[34]++; // House Party
    mSaveInfo.missionStatus[48]++; // Wu Zi Mu and Farewell My Love
    mSaveInfo.missionStatus[59]++; // Jizzy

    for (auto i : exceptions)
        {
            mSaveInfo.missionStatus[i] = 0;
        }
}

/*******************************************************/
void
MissionRandomizer::Save ()
{
    CGenericGameStorage::SaveDataToWorkBuffer (&mSaveInfo, sizeof (mSaveInfo));
}

/*******************************************************/
void
MissionRandomizer::Load ()
{
    auto config = ConfigManager::GetInstance ()->GetConfigs ().missions;
    MissionRandomizerSaveStructure saveInfo;
    CGenericGameStorage::LoadDataFromWorkBuffer (&saveInfo, sizeof (saveInfo));

    ResetSaveData ();
    if (std::string (saveInfo.signature, 11) != "RAINBOMIZER")
        return InitShuffledMissionOrder ();

    mSaveInfo.randomSeed = saveInfo.randomSeed;
    Logger::GetLogger ()->LogMessage ("Setting seed "
                                      + std::to_string (mSaveInfo.randomSeed)
                                      + " from save file");
    if (config.forceShufflingSeed && config.shufflingSeed != -1)
        mSaveInfo.randomSeed = config.shufflingSeed;

    InitShuffledMissionOrder ();

    mSaveInfo = saveInfo;
}

/*******************************************************/
void
ScriptByPassCheat ()
{
    if (MissionRandomizer::GetInstance ()->mRandomizedScript)
        MissionRandomizer::GetInstance ()->SetScriptByPass ();
}

/*******************************************************/
void
BringBackMyMarkersCheat ()
{
    ScriptSpace[409] = 0;
}

/*******************************************************/
void
MissionRandomizer::InstallCheat (void *func, uint32_t hash)
{
    const int total_cheats    = 92;
    void **   cheat_functions = reinterpret_cast<void **> (0x8A5B58);
    int *     cheat_hashes    = reinterpret_cast<int *> (0x8A5CC8);

    for (int i = 0; i < total_cheats; i++)
        {
            if (cheat_hashes[i] == 0)
                {
                    cheat_hashes[i]    = hash;
                    cheat_functions[i] = func;
                    Logger::GetLogger ()->LogMessage (
                        "Successfully registered cheat "
                        + std::to_string (hash));
                    return;
                }
        }

    Logger::GetLogger ()->LogMessage ("Failed to register cheat "
                                      + std::to_string (hash));
    return;
}

/*******************************************************/
void __fastcall OverrideHospitalEndPosition (CRunningScript *scr)
{
    HookManager::CallOriginal<injector::thiscall<void (CRunningScript *)>,
                              0x469F5B> (scr);

    auto missionRandomizer = MissionRandomizer::GetInstance ();
    if (scr->m_bWastedOrBusted && scr == missionRandomizer->mRandomizedScript)
        {
            int i = missionRandomizer->mOriginalMissionNumber;
            if (missionStartPos.count (i))
                {
                    auto pos = missionStartPos[i];
                    Scrpt::CallOpcode (0x9FF, "set_restart_closest_to", pos.x,
                                       pos.y, pos.z);
                }
        }
}

/*******************************************************/
bool
MissionRandomizer::VerifyMainSCM()
{
    const size_t MAIN_SIZE = 3079599;
    bool valid = false;
    
    FILE *mainScm
        = fopen (GetGameDirRelativePathA ("data/script/main.scm"), "rb");

    if(!mainScm)
        return true; // Can't be certain if it's valid or not so continue anyway
                     // for convenience
    
    fseek(mainScm, SEEK_SET, SEEK_END);

    valid = ftell(mainScm) == MAIN_SIZE;
    fclose(mainScm);

    if(!valid)
        Logger::GetLogger ()->LogMessage (
            "main.scm is invalid size: expected: 3079599");

    return valid;
}

/*******************************************************/
void
MissionRandomizer::Initialise ()
{

    auto config = ConfigManager::GetInstance ()->GetConfigs ().missions;

    if (!config.enabled || (!config.disableMainScmCheck && !VerifyMainSCM()))
        return;

    if (!mTempMissionData)
        mTempMissionData = new unsigned char[69000];
    if (!mLocalVariables)
        mLocalVariables = new int[1024];

    RegisterHooks (
        {{HOOK_CALL, 0x489929, (void *) &RandomizeMissionToStart},
         {HOOK_CALL, 0x489A7A, (void *) &StoreRandomizedScript},
         {HOOK_CALL, 0x441869, (void *) &UnlockCities},
         {HOOK_CALL, 0x4417F5, (void *) &UnlockCities},
         {HOOK_CALL, 0x60C943, (void *) &UnlockCities},
         {HOOK_CALL, 0x60C95D, (void *) &UnlockCities},
         {HOOK_CALL, 0x5D15A6, (void *) &SaveMissionData},
         {HOOK_CALL, 0x5D19CE, (void *) &LoadMissionData},
         {HOOK_CALL, 0x60C925, (void *) &CorrectMaxNumberOfGroupMembers}});

    RegisterDelayedHooks (
        {{HOOK_CALL, 0x469FB0, (void *) &JumpOnMissionEnd},
         {HOOK_CALL, 0x53BE76, (void *) &InitAtNewGame},
         {HOOK_CALL, 0x469F5B, (void *) &OverrideHospitalEndPosition}});

    RegisterDelayedFunction ([] { injector::MakeNOP (0x469fb5, 2); });

    this->ResetSaveData ();
    this->InitShuffledMissionOrder ();

    this->InstallCheat ((void *) &ScriptByPassCheat, 0xBB5ADFD7);
    this->InstallCheat ((void *) &BringBackMyMarkersCheat, 0xFD7F4A34);

    Logger::GetLogger ()->LogMessage ("Intialised MissionRandomizer");
}

/*******************************************************/
void
MissionRandomizer::InitShuffledMissionOrder ()
{
    auto config = ConfigManager::GetInstance ()->GetConfigs ().missions;
    mShuffledOrder.clear ();
    if (!config.shufflingEnabled)
        return;

    std::mt19937 engine{mSaveInfo.randomSeed};

    std::vector<uint8_t> remainingMissions;
    int                  index = START_MISSIONS;
    for (auto i : mSaveInfo.missionStatus.data)
        {
            for (int j = 0; j < i; j++)
                remainingMissions.push_back (index);
            index++;
        }

    index = START_MISSIONS;
    for (auto i : mSaveInfo.missionStatus.data)
        {
            for (int j = 0; j < i; j++)
                {
                    if (remainingMissions.size () <= 0)
                        continue;

                    std::uniform_int_distribution<unsigned int> dist{
                        0, remainingMissions.size () - 1};

                    auto randomMission = dist (engine);
                    mShuffledOrder[index].push_back (
                        remainingMissions[randomMission]);

                    remainingMissions.erase (remainingMissions.begin ()
                                             + randomMission);
                }
            index++;
        }
}

/*******************************************************/
void
MissionRandomizer::DestroyInstance ()
{
    if (MissionRandomizer::mInstance)
        delete MissionRandomizer::mInstance;
}

/*******************************************************/
MissionRandomizer *
MissionRandomizer::GetInstance ()
{
    if (!MissionRandomizer::mInstance)
        {
            MissionRandomizer::mInstance = new MissionRandomizer ();
            atexit (&MissionRandomizer::DestroyInstance);
        }
    return MissionRandomizer::mInstance;
}
