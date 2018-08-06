/*
 * Fuzzy.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
*/
#pragma once
#include "fl/Headers.h"
#include "Goals.h"

class VCAI;
class CArmedInstance;
class CBank;
struct SectorMap;

class engineBase
{
protected:
	fl::Engine engine;
	fl::RuleBlock rules;
	virtual void configure();
	void addRule(const std::string & txt);
public:
	engineBase();
};

class TacticalAdvantageEngine : public engineBase
{
public:
	TacticalAdvantageEngine();

	float getTacticalAdvantage(const CArmedInstance * we, const CArmedInstance * enemy); //returns factor how many times enemy is stronger than us
private:
	std::unique_ptr<fl::InputVariable> ourWalkers, ourShooters, ourFlyers, enemyWalkers, enemyShooters, enemyFlyers;
	std::unique_ptr<fl::InputVariable> ourSpeed, enemySpeed;
	std::unique_ptr<fl::InputVariable> bankPresent;
	std::unique_ptr<fl::InputVariable> castleWalls;
	std::unique_ptr<fl::OutputVariable> threat;
};

class HeroMovementGoalEngineBase : public engineBase //in future - maybe derive from some (GoalEngineBase : public engineBase) class for handling non-movement goals with common utility for goal engines
{
public:
	HeroMovementGoalEngineBase();

	virtual float evaluate(Goals::AbstractGoal & goal) = 0;

protected:
	void setSharedFuzzyVariables(Goals::AbstractGoal & goal);

	std::unique_ptr<fl::InputVariable> strengthRatio;
	std::unique_ptr<fl::InputVariable> heroStrength;
	std::unique_ptr<fl::InputVariable> turnDistance;
	std::unique_ptr<fl::InputVariable> missionImportance;
	std::unique_ptr<fl::OutputVariable> value;

private:
	float calculateTurnDistanceInputValue(const CGHeroInstance * h, int3 tile) const;
};

class VisitTileEngine : public HeroMovementGoalEngineBase
{
public:
	VisitTileEngine();
	float evaluate(Goals::AbstractGoal & goal) override;
};

class GetObjEngine : public HeroMovementGoalEngineBase
{
public:
	GetObjEngine();
	float evaluate(Goals::AbstractGoal & goal) override;
protected:
	std::unique_ptr<fl::InputVariable> objectValue;
};

class FuzzyHelper
{
	friend class VCAI;

public:
	TacticalAdvantageEngine tacticalAdvantageEngine;
	VisitTileEngine visitTileEngine;
	GetObjEngine getObjEngine;

	float evaluate(Goals::Explore & g);
	float evaluate(Goals::RecruitHero & g);
	float evaluate(Goals::VisitTile & g);
	float evaluate(Goals::GetObj & g);
	float evaluate(Goals::VisitHero & g);
	float evaluate(Goals::BuildThis & g);
	float evaluate(Goals::DigAtTile & g);
	float evaluate(Goals::CollectRes & g);
	float evaluate(Goals::Build & g);
	float evaluate(Goals::BuyArmy & g);
	float evaluate(Goals::GatherArmy & g);
	float evaluate(Goals::ClearWayTo & g);
	float evaluate(Goals::Invalid & g);
	float evaluate(Goals::AbstractGoal & g);
	void setPriority(Goals::TSubgoal & g);

	ui64 estimateBankDanger(const CBank * bank); //TODO: move to another class?

	Goals::TSubgoal chooseSolution(Goals::TGoalVec vec);
	//std::shared_ptr<AbstractGoal> chooseSolution (std::vector<std::shared_ptr<AbstractGoal>> & vec);
};
