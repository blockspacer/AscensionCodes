#include "StdAfx.h"
#include "PlayerAnimations.h"

#include "Player/Player.h"
#include "Player/Input/PlayerInput.h"

#include "Actions/MoveForceAction.h"

#include <CryAnimation/ICryAnimation.h>
#include <ICryMannequin.h>

CPlayerAnimations::CPlayerAnimations()
	: m_pActionController(nullptr)
	, m_pAnimationContext(nullptr)
{
}

CPlayerAnimations::~CPlayerAnimations()
{
	SAFE_RELEASE(m_pActionController);
	SAFE_DELETE(m_pAnimationContext);
}

void CPlayerAnimations::PostInit(IGameObject *pGameObject)
{
	m_pPlayer = static_cast<CPlayer *>(pGameObject->QueryExtension("Player"));

	// Make sure that this extension is updated regularly via the Update function below
	pGameObject->EnableUpdateSlot(this, 0);
}

void CPlayerAnimations::Update(SEntityUpdateContext& ctx, int updateSlot)
{
	// Start updating the motion parameters used for blend spaces
	if (auto *pPhysEnt = m_pPlayer->GetEntity()->GetPhysics())
	{
		// Update entity rotation as the player turns
		// Start with getting the look orientation's yaw, pitch and roll
		Ang3 ypr = CCamera::CreateAnglesYPR(Matrix33(m_pPlayer->GetInput()->GetLookOrientation()));

		// We only want to affect Z-axis rotation, zero pitch and roll
		ypr.y = 0;
		ypr.z = 0;

		// Re-calculate the quaternion based on the corrected look orientation
		Quat correctedOrientation = Quat(CCamera::CreateOrientationYPR(ypr));

		auto *pCharacter = m_pPlayer->GetEntity()->GetCharacter(CPlayer::eGeometry_ThirdPerson);

		// Get the player's velocity from physics
		pe_status_dynamics playerDynamics;
		if (pPhysEnt->GetStatus(&playerDynamics) != 0 && pCharacter != nullptr)
		{
			// Set turn rate as the difference between previous and new entity rotation
			//float turnAngle = Ang3::CreateRadZ(GetEntity()->GetForwardDir(), correctedOrientation.GetColumn1()) / ctx.fFrameTime;
			float turnAngle = 0.0f;
			float travelAngle = Ang3::CreateRadZ(GetEntity()->GetForwardDir(), playerDynamics.v.GetNormalized());
			float travelSpeed = playerDynamics.v.GetLength2D();

			// Set the travel speed based on the physics velocity magnitude
			// Keep in mind that the maximum number for motion parameters is 10.
			// If your velocity can reach a magnitude higher than this, divide by the maximum theoretical account and work with a 0 - 1 ratio.
			pCharacter->GetISkeletonAnim()->SetDesiredMotionParam(eMotionParamID_TravelSpeed, travelSpeed, 0.f);

			// Update the turn speed in CryAnimation, note that the maximum motion parameter (10) applies here too.
			//pCharacter->GetISkeletonAnim()->SetDesiredMotionParam(eMotionParamID_TurnAngle, turnAngle, 0.f);
			pCharacter->GetISkeletonAnim()->SetDesiredMotionParam(eMotionParamID_TravelAngle, travelAngle, 0.f);

			if (m_pPlayer->GetMovement()->IsOnGround())
			{
				// Calculate slope value
				Vec3 groundNormal = m_pPlayer->GetMovement()->GetGroundNormal() * correctedOrientation;
				groundNormal.x = 0.0f;
				float cosine = Vec3Constants<float>::fVec3_OneZ | groundNormal;
				Vec3 sine = Vec3Constants<float>::fVec3_OneZ % groundNormal;

				float travelSlope = atan2f(sgn(sine.x) * sine.GetLength(), cosine);

				pCharacter->GetISkeletonAnim()->SetDesiredMotionParam(eMotionParamID_TravelSlope, travelSlope, 0.f);
			}
		}

		// Send updated transform to the entity, only orientation changes
		//GetEntity()->SetPosRotScale(GetEntity()->GetWorldPos(), correctedOrientation, Vec3(1, 1, 1));
	}

	if (m_pActionController != nullptr)
	{
		m_pActionController->Update(ctx.fFrameTime);
	}
}

void CPlayerAnimations::ProcessEvent(SEntityEvent& event)
{
	switch (event.event)
	{
		case ENTITY_EVENT_ANIM_EVENT:
		{
			const AnimEventInstance *pAnimEvent = reinterpret_cast<const AnimEventInstance *>(event.nParam[0]);
			ICharacterInstance *pCharacter = reinterpret_cast<ICharacterInstance *>(event.nParam[1]);

			m_pActionController->OnAnimationEvent(pCharacter, *pAnimEvent);
		}
		break;
	}
}

void CPlayerAnimations::OnPlayerModelChanged()
{
	// Release previous controller and context, if we are respawning
	SAFE_RELEASE(m_pActionController);
	SAFE_DELETE(m_pAnimationContext);

	// Now start loading the Mannequin data
	IMannequin &mannequinInterface = gEnv->pGameFramework->GetMannequinInterface();
	IAnimationDatabaseManager &animationDatabaseManager = mannequinInterface.GetAnimationDatabaseManager();

	const char *mannequinControllerDefinition = m_pPlayer->GetCVars().m_pThirdPersonControllerDefinition->GetString();

	// Load the Mannequin controller definition.
	// This is owned by the animation database manager
	const SControllerDef *pControllerDefinition = animationDatabaseManager.LoadControllerDef(mannequinControllerDefinition);
	if (pControllerDefinition == nullptr)
	{
		CryWarning(VALIDATOR_MODULE_GAME, VALIDATOR_ERROR, "Failed to load controller definition.");
		return;
	}

	const char *mannequinContextName = m_pPlayer->GetCVars().m_pThirdPersonMannequinContext->GetString();
	const char *animationDatabasePath = m_pPlayer->GetCVars().m_pThirdPersonAnimationDatabase->GetString();

	// Load the animation database
	auto *pAnimationDatabase = animationDatabaseManager.Load(animationDatabasePath);
	if (pAnimationDatabase == nullptr)
	{
		CryWarning(VALIDATOR_MODULE_GAME, VALIDATOR_ERROR, "Failed to load animation database %s!", animationDatabasePath);
		return;
	}

	// Create a new animation context for the controller definition we loaded above
	m_pAnimationContext = new SAnimationContext(*pControllerDefinition);

	// Now create the controller that will be handling animation playback
	m_pActionController = mannequinInterface.CreateActionController(GetEntity(), *m_pAnimationContext);
	CRY_ASSERT(m_pActionController != nullptr);

	ICharacterInstance *pCharacterInstance = GetEntity()->GetCharacter(CPlayer::eGeometry_ThirdPerson);
	CRY_ASSERT(pCharacterInstance != nullptr);

	// Activate the Main context we'll be playing our animations in
	ActivateMannequinContext(mannequinContextName, *pCharacterInstance, *pControllerDefinition, *pAnimationDatabase);

	// Create this idle fragment
	// This implementation handles switching to various sub-fragments by itself, based on input and physics data
	int priority = 0;
	auto idleFragmentId = pControllerDefinition->m_fragmentIDs.Find("MotionIdle");

	m_pIdleFragment = new TAction<SAnimationContext>(priority, idleFragmentId, TAG_STATE_EMPTY, 0);

	// Queue the idle fragment to start playing immediately on next update
	m_pActionController->Queue(*m_pIdleFragment.get());

	// Acquire tag identifiers to avoid doing so each update
	m_rotateTagId = m_pAnimationContext->state.GetDef().Find("MotionTurn");
	m_walkTagId = m_pAnimationContext->state.GetDef().Find("MotionMovement");

	// Disable movement coming from the animation (root joint offset), we control this entirely via physics
	pCharacterInstance->GetISkeletonAnim()->SetAnimationDrivenMotion(1);
}

void CPlayerAnimations::ActivateMannequinContext(const char *contextName, ICharacterInstance &character, const SControllerDef &controllerDefinition, const IAnimationDatabase &animationDatabase)
{
	IEntity &entity = *GetEntity();

	const TagID scopeContextId = controllerDefinition.m_scopeContexts.Find(contextName);
	if (scopeContextId == TAG_ID_INVALID)
	{
		CryWarning(VALIDATOR_MODULE_GAME, VALIDATOR_ERROR, "Failed to find %s scope context id in controller definition.", contextName);
		return;
	}

	// Setting Scope contexts can happen at any time, and what entity or character instance we have bound to a particular scope context
	// can change during the lifetime of an action controller.
	m_pActionController->SetScopeContext(scopeContextId, entity, &character, &animationDatabase);
}

//------------------------------------------------------------------------
void CPlayerAnimations::SetTagGroup(TagGroupID groupId, TagID tagId)
{
	if (m_pActionController)
	{
		SAnimationContext &animContext = m_pActionController->GetContext();

		animContext.state.SetGroup(groupId, tagId);
	}
}
void CPlayerAnimations::ClearTagGroup(TagGroupID groupId)
{
	if (m_pActionController)
	{
		SAnimationContext &animContext = m_pActionController->GetContext();

		animContext.state.ClearGroup(groupId);
	}
}

//------------------------------------------------------------------------
void CPlayerAnimations::PlayMoveAction(FragmentID fragmentID, EPlayerActionPriority priority, TagState fragTags)
{
	if (m_pActionController)
	{
		m_pLastAction = new CMoveAction(m_pPlayer->GetEntity(), priority, fragmentID, fragTags);
		m_pActionController->Queue(*m_pLastAction);
	}
}
void CPlayerAnimations::PlayMoveForceAction(FragmentID fragmentID, EPlayerActionPriority priority, TagState fragTags)
{
	if (m_pActionController)
	{
		m_pLastAction = new CMoveForceAction(m_pPlayer->GetEntity(), priority, fragmentID, fragTags);
		m_pActionController->Queue(*m_pLastAction);
	}
}

//------------------------------------------------------------------------
void CPlayerAnimations::PlayFragment(FragmentID fragmentID, EPlayerActionPriority priority, TagState tagState)
{
	if (m_pActionController)
	{
		m_pLastAction = new TPlayerAction(priority, fragmentID, tagState);
		m_pActionController->Queue(*m_pLastAction);
	}
}
void CPlayerAnimations::PlayFragment(char * fragmentName, EPlayerActionPriority priority, TagState tagState)
{
	if (m_pActionController)
	{
		FragmentID fragID = m_pActionController->GetContext().controllerDef.m_fragmentIDs.Find(fragmentName);
		m_pLastAction = new TPlayerAction(priority, fragID, tagState);
		m_pActionController->Queue(*m_pLastAction);
	}
}

//------------------------------------------------------------------------
void CPlayerAnimations::ForceFinishLastAction()
{
	if (m_pLastAction)
		m_pLastAction->ForceFinish();
}