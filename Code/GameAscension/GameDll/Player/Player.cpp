#include "StdAfx.h"
#include "Player.h"

#include "Input/PlayerInput.h"
#include "View/PlayerView.h"
#include "Animations/PlayerAnimations.h"
#include "States/PlayerStateManager.h"
#include "Weapons/PlayerSword.h"

#include "GamePlugin.h"
#include "Game/GameRules.h"

#include "Entities/Gameplay/SpawnPoint.h"

#include <CryRenderer/IRenderAuxGeom.h>

class CPlayerRegistrator
	: public IEntityRegistrator
{
	virtual void Register() override
	{
		CGamePlugin::RegisterEntityWithDefaultComponent<CPlayer>("Player");

		CGamePlugin::RegisterEntityComponent<CPlayerInput>("PlayerInput");
		CGamePlugin::RegisterEntityComponent<CPlayerView>("PlayerView");
		CGamePlugin::RegisterEntityComponent<CPlayerAnimations>("PlayerAnimations");
		CGamePlugin::RegisterEntityComponent<CPlayerStateManager>("PlayerStateManager");
		CGamePlugin::RegisterEntityComponent<CPlayerSword>("PlayerSword");
	}

	virtual void Unregister() override
	{
	}
};

CPlayerRegistrator g_playerRegistrator;

CPlayer::CPlayer()
	: m_pInput(nullptr)
	, m_pView(nullptr)
	, m_bAlive(false)
	, m_bIsSpaceKey(false)
	, m_weaponType(ewt_magic)
{
	RegisterCVars();
}

CPlayer::~CPlayer()
{
	UnregisterCVars();
	gEnv->pGameFramework->GetIActorSystem()->RemoveActor(GetEntityId());
}

void CPlayer::RegisterCVars()
{
	REGISTER_CVAR2("pl_mass", &m_mass, 90.f, VF_CHEAT, "Mass of the player entity in kg");
	REGISTER_CVAR2("pl_walkSpeed", &m_walkSpeed, 1.8f, VF_CHEAT, "Player walking speed");
	REGISTER_CVAR2("pl_runSpeed", &m_runSpeed, 3.7f, VF_CHEAT, "Player running speed");
	REGISTER_CVAR2("pl_rotationSpeed", &m_turnSpeed, 7.0f, VF_CHEAT, "Player rotation speed while moving");

	REGISTER_CVAR2("pl_rotationSpeedYaw", &m_playerViewParams.m_rotationSpeedYaw, 0.05f, VF_CHEAT, "Speed at which the player rotates entity yaw");
	REGISTER_CVAR2("pl_rotationSpeedPitch", &m_playerViewParams.m_rotationSpeedPitch, 0.05f, VF_CHEAT, "Speed at which the player rotates entity pitch");

	REGISTER_CVAR2("pl_rotationLimitsMinPitch", &m_playerViewParams.m_rotationLimitsMinPitch, -0.84f, VF_CHEAT, "Minimum entity pitch limit");
	REGISTER_CVAR2("pl_rotationLimitsMaxPitch", &m_playerViewParams.m_rotationLimitsMaxPitch, 1.5f, VF_CHEAT, "Maximum entity pitch limit");

	REGISTER_CVAR2("pl_eyeHeight", &m_actorEyeHeight, 0.935f, VF_CHEAT, "Height of the player's eyes from ground");
}

void CPlayer::UnregisterCVars()
{
	IConsole* pConsole = gEnv->pConsole;
	if (pConsole)
	{
		pConsole->UnregisterVariable("pl_mass");
		pConsole->UnregisterVariable("pl_walkSpeed");
		pConsole->UnregisterVariable("pl_runSpeed");
		pConsole->UnregisterVariable("pl_rotationSpeed");
		pConsole->UnregisterVariable("pl_rotationSpeedYaw");
		pConsole->UnregisterVariable("pl_rotationSpeedPitch");
		pConsole->UnregisterVariable("pl_rotationLimitsMinPitch");
		pConsole->UnregisterVariable("pl_rotationLimitsMaxPitch");
		pConsole->UnregisterVariable("pl_eyeHeight");
	}
}

void CPlayer::SelectWeapon(EWeaponType weaponType, bool isForce)
{
	if (isForce || (m_weaponType != weaponType))
	{
		m_weaponType = weaponType;
		m_pAnimations->SetWeaponTag(weaponType);

		if (weaponType == ewt_sword)
		{
			m_pAnimations->PlayFragment("SelectSword", PP_Sword);
		}
		else
		{
			m_pAnimations->PlayFragment("DeSelectSword", PP_Sword);
		}
	}
}

bool CPlayer::Init(IGameObject *pGameObject)
{
	SetGameObject(pGameObject);

	return pGameObject->BindToNetwork();
}

void CPlayer::PostInit(IGameObject *pGameObject)
{
	m_pAnimations = static_cast<CPlayerAnimations *>(GetGameObject()->AcquireExtension("PlayerAnimations"));
	m_pInput = static_cast<CPlayerInput *>(GetGameObject()->AcquireExtension("PlayerInput"));
	m_pStateManager = static_cast<CPlayerStateManager *>(GetGameObject()->AcquireExtension("PlayerStateManager"));
	m_pSword = static_cast<CPlayerSword *>(GetGameObject()->AcquireExtension("PlayerSword"));

	m_pView = static_cast<CPlayerView *>(GetGameObject()->AcquireExtension("PlayerView"));

	// Register with the actor system
	gEnv->pGameFramework->GetIActorSystem()->AddActor(GetEntityId(), this);
}

void CPlayer::ProcessEvent(SEntityEvent& event)
{
	switch (event.event)
	{
	case ENTITY_EVENT_RESET:
	{
		switch (event.nParam[0])
		{
		case 0: // Game ends
			break;
		case 1: // Game starts
				// Make sure to revive player when respawning in Editor

			// Set camera X and Y rotation to 0 angle while initializing.
			GetEntity()->SetRotation(Quat::CreateRotationZ(GetEntity()->GetRotation().GetRotZ()));

			SetHealth(GetMaxHealth());
			break;
		default:
			break;
		}
	}
	break;
	}
}

void CPlayer::SetHealth(float health)
{
	// Find a spawn point and move the entity there
	SelectSpawnPoint();

	// Note that this implementation does not handle the concept of death, SetHealth(0) will still revive the player.
	if (m_bAlive)
		return;

	m_bAlive = true;

	// Unhide the entity in case hidden by the Editor
	GetEntity()->Hide(false);

	// Make sure that the player spawns upright
	GetEntity()->SetWorldTM(Matrix34::Create(Vec3(1, 1, 1), IDENTITY, GetEntity()->GetWorldPos()));

	// Set the player geometry, this also triggers physics proxy creation
	SetPlayerModel();

	// Notify input that the player respawned
	m_pInput->OnPlayerRespawn();
}

float CPlayer::GetMoveSpeed() const
{
	return m_bIsSpaceKey ? GetCVars().m_runSpeed : GetCVars().m_walkSpeed;
}

void CPlayer::SwordAttack()
{
	if (m_weaponType == ewt_sword)
		m_pSword->SwordAttack();
}

void CPlayer::SelectSpawnPoint()
{
	// We only handle default spawning below for the Launcher
	// Editor has special logic in CEditorGame
	if (gEnv->IsEditor())
		return;

	// Spawn at first default spawner
	auto *pEntityIterator = gEnv->pEntitySystem->GetEntityIterator();
	pEntityIterator->MoveFirst();

	auto *pSpawnerClass = gEnv->pEntitySystem->GetClassRegistry()->FindClass("SpawnPoint");
	auto extensionId = gEnv->pGameFramework->GetIGameObjectSystem()->GetID("SpawnPoint");

	while (!pEntityIterator->IsEnd())
	{
		IEntity *pEntity = pEntityIterator->Next();

		if (pEntity->GetClass() != pSpawnerClass)
			continue;

		auto *pGameObject = gEnv->pGameFramework->GetGameObject(pEntity->GetId());
		if (pGameObject == nullptr)
			continue;

		auto *pSpawner = static_cast<CSpawnPoint *>(pGameObject->QueryExtension(extensionId));
		if (pSpawner == nullptr)
			continue;

		pSpawner->SpawnEntity(*GetEntity());

		break;
	}
}

void CPlayer::Physicalize()
{
	// Physicalize the player as type Living.
	// This physical entity type is specifically implemented for players
	SEntityPhysicalizeParams physParams;
	physParams.type = PE_LIVING;

	physParams.mass = GetCVars().m_mass;

	pe_player_dimensions playerDimensions;

	// Prefer usage of a cylinder instead of capsule
	playerDimensions.bUseCapsule = 0;

	// Specify the size of our cylinder
	playerDimensions.sizeCollider = Vec3(0.45f, 0.45f, GetCVars().m_playerEyeHeight * 0.5f);

	// Keep pivot at the player's feet (defined in player geometry) 
	playerDimensions.heightPivot = 0.f;
	// Offset collider upwards
	playerDimensions.heightCollider = 1.f;
	playerDimensions.groundContactEps = 0.004f;

	physParams.pPlayerDimensions = &playerDimensions;

	pe_player_dynamics playerDynamics;
	playerDynamics.kAirControl = 0.f;
	playerDynamics.mass = physParams.mass;

	physParams.pPlayerDynamics = &playerDynamics;

	GetEntity()->Physicalize(physParams);
}


void CPlayer::SetPlayerModel()
{
	// Load the third person model
	GetEntity()->LoadCharacter(eGeometry_ThirdPerson, GetCVars().m_pThirdPersonGeometry->GetString());
	
	// Do the same for animations so that Mannequin data can be initialized
	m_pAnimations->OnPlayerModelChanged();

	// Now create the physical representation of the entity
	Physicalize();
}
