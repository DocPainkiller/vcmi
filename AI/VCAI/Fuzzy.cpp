/*
 * Fuzzy.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
*/
#include "StdInc.h"
#include "Fuzzy.h"
#include <limits>

#include "../../lib/mapObjects/MapObjects.h"
#include "../../lib/mapObjects/CommonConstructors.h"
#include "../../lib/CCreatureHandler.h"
#include "../../lib/CPathfinder.h"
#include "../../lib/CGameStateFwd.h"
#include "../../lib/VCMI_Lib.h"
#include "../../CCallback.h"
#include "VCAI.h"
#include "MapObjectsEvaluator.h"

#define MIN_AI_STRENGHT (0.5f) //lower when combat AI gets smarter
#define UNGUARDED_OBJECT (100.0f) //we consider unguarded objects 100 times weaker than us

struct BankConfig;
class CBankInfo;
class Engine;
class InputVariable;
class CGTownInstance;

FuzzyHelper * fh;

extern boost::thread_specific_ptr<CCallback> cb;
extern boost::thread_specific_ptr<VCAI> ai;

engineBase::engineBase()
{
	engine.addRuleBlock(&rules);
}

void engineBase::configure()
{
	engine.configure("Minimum", "Maximum", "Minimum", "AlgebraicSum", "Centroid", "General");
	logAi->info(engine.toString());
}

void engineBase::addRule(const std::string & txt)
{
	rules.addRule(fl::Rule::parse(txt, &engine));
}

struct armyStructure
{
	float walkers, shooters, flyers;
	ui32 maxSpeed;
};

armyStructure evaluateArmyStructure(const CArmedInstance * army)
{
	ui64 totalStrenght = army->getArmyStrength();
	double walkersStrenght = 0;
	double flyersStrenght = 0;
	double shootersStrenght = 0;
	ui32 maxSpeed = 0;

	for(auto s : army->Slots())
	{
		bool walker = true;
		if(s.second->type->hasBonusOfType(Bonus::SHOOTER))
		{
			shootersStrenght += s.second->getPower();
			walker = false;
		}
		if(s.second->type->hasBonusOfType(Bonus::FLYING))
		{
			flyersStrenght += s.second->getPower();
			walker = false;
		}
		if(walker)
			walkersStrenght += s.second->getPower();

		vstd::amax(maxSpeed, s.second->type->valOfBonuses(Bonus::STACKS_SPEED));
	}
	armyStructure as;
	as.walkers = walkersStrenght / totalStrenght;
	as.shooters = shootersStrenght / totalStrenght;
	as.flyers = flyersStrenght / totalStrenght;
	as.maxSpeed = maxSpeed;
	assert(as.walkers || as.flyers || as.shooters);
	return as;
}

float HeroMovementGoalEngineBase::calculateTurnDistanceInputValue(const CGHeroInstance * h, int3 tile) const
{
	float turns = 0.0f;
	float distance = CPathfinderHelper::getMovementCost(h, tile);
	if(distance)
	{
		if(distance < h->movement) //we can move there within one turn
			turns = (fl::scalar)distance / h->movement;
		else
			turns = 1 + (fl::scalar)(distance - h->movement) / h->maxMovePoints(true); //bool on land?
	}
	return turns;
}

ui64 FuzzyHelper::estimateBankDanger(const CBank * bank)
{
	//this one is not fuzzy anymore, just calculate weighted average

	auto objectInfo = VLC->objtypeh->getHandlerFor(bank->ID, bank->subID)->getObjectInfo(bank->appearance);

	CBankInfo * bankInfo = dynamic_cast<CBankInfo *>(objectInfo.get());

	ui64 totalStrength = 0;
	ui8 totalChance = 0;
	for(auto config : bankInfo->getPossibleGuards())
	{
		totalStrength += config.second.totalStrength * config.first;
		totalChance += config.first;
	}
	return totalStrength / std::max<ui8>(totalChance, 1); //avoid division by zero

}

TacticalAdvantageEngine::TacticalAdvantageEngine()
{
	try
	{
		ourShooters = vstd::make_unique<fl::InputVariable>(fl::InputVariable("OurShooters"));
		ourWalkers = vstd::make_unique<fl::InputVariable>(fl::InputVariable("OurWalkers"));
		ourFlyers = vstd::make_unique<fl::InputVariable>(fl::InputVariable("OurFlyers"));
		enemyShooters = vstd::make_unique<fl::InputVariable>(fl::InputVariable("EnemyShooters"));
		enemyWalkers = vstd::make_unique<fl::InputVariable>(fl::InputVariable("EnemyWalkers"));
		enemyFlyers = vstd::make_unique<fl::InputVariable>(fl::InputVariable("EnemyFlyers"));

		for(auto val : {ourShooters.get(), ourWalkers.get(), ourFlyers.get(), enemyShooters.get(), enemyWalkers.get(), enemyFlyers.get()})
		{
			engine.addInputVariable(val);
			val->addTerm(new fl::Ramp("FEW", 0.6, 0.0));
			val->addTerm(new fl::Ramp("MANY", 0.4, 1));
			val->setRange(0.0, 1.0);
		}

		ourSpeed = vstd::make_unique<fl::InputVariable>(fl::InputVariable("OurSpeed"));
		enemySpeed = vstd::make_unique<fl::InputVariable>(fl::InputVariable("EnemySpeed"));

		for(auto val : {ourSpeed.get(), enemySpeed.get()})
		{
			engine.addInputVariable(val);
			val->addTerm(new fl::Ramp("LOW", 6.5, 3));
			val->addTerm(new fl::Triangle("MEDIUM", 5.5, 10.5));
			val->addTerm(new fl::Ramp("HIGH", 8.5, 16));
			val->setRange(0, 25);
		}

		castleWalls = vstd::make_unique<fl::InputVariable>(fl::InputVariable("CastleWalls"));
		engine.addInputVariable(castleWalls.get());
		{
			fl::Rectangle * none = new fl::Rectangle("NONE", CGTownInstance::NONE, CGTownInstance::NONE + (CGTownInstance::FORT - CGTownInstance::NONE) * 0.5f);
			castleWalls->addTerm(none);

			fl::Trapezoid * medium = new fl::Trapezoid("MEDIUM", (CGTownInstance::FORT - CGTownInstance::NONE) * 0.5f, CGTownInstance::FORT,
				CGTownInstance::CITADEL, CGTownInstance::CITADEL + (CGTownInstance::CASTLE - CGTownInstance::CITADEL) * 0.5f);
			castleWalls->addTerm(medium);

			fl::Ramp * high = new fl::Ramp("HIGH", CGTownInstance::CITADEL - 0.1, CGTownInstance::CASTLE);
			castleWalls->addTerm(high);

			castleWalls->setRange(CGTownInstance::NONE, CGTownInstance::CASTLE);
		}


		bankPresent = vstd::make_unique<fl::InputVariable>(fl::InputVariable("Bank"));
		engine.addInputVariable(bankPresent.get());
		{
			fl::Rectangle * termFalse = new fl::Rectangle("FALSE", 0.0, 0.5f);
			bankPresent->addTerm(termFalse);
			fl::Rectangle * termTrue = new fl::Rectangle("TRUE", 0.5f, 1);
			bankPresent->addTerm(termTrue);
			bankPresent->setRange(0, 1);
		}

		threat = vstd::make_unique<fl::OutputVariable>(fl::OutputVariable("Threat"));
		engine.addOutputVariable(threat.get());
		threat->addTerm(new fl::Ramp("LOW", 1, MIN_AI_STRENGHT));
		threat->addTerm(new fl::Triangle("MEDIUM", 0.8, 1.2));
		threat->addTerm(new fl::Ramp("HIGH", 1, 1.5));
		threat->setRange(MIN_AI_STRENGHT, 1.5);

		addRule("if OurShooters is MANY and EnemySpeed is LOW then Threat is LOW");
		addRule("if OurShooters is MANY and EnemyShooters is FEW then Threat is LOW");
		addRule("if OurSpeed is LOW and EnemyShooters is MANY then Threat is HIGH");
		addRule("if OurSpeed is HIGH and EnemyShooters is MANY then Threat is LOW");

		addRule("if OurWalkers is FEW and EnemyShooters is MANY then Threat is somewhat LOW");
		addRule("if OurShooters is MANY and EnemySpeed is HIGH then Threat is somewhat HIGH");
		//just to cover all cases
		addRule("if OurShooters is FEW and EnemySpeed is HIGH then Threat is MEDIUM");
		addRule("if EnemySpeed is MEDIUM then Threat is MEDIUM");
		addRule("if EnemySpeed is LOW and OurShooters is FEW then Threat is MEDIUM");

		addRule("if Bank is TRUE and OurShooters is MANY then Threat is somewhat HIGH");
		addRule("if Bank is TRUE and EnemyShooters is MANY then Threat is LOW");

		addRule("if CastleWalls is HIGH and OurWalkers is MANY then Threat is very HIGH");
		addRule("if CastleWalls is HIGH and OurFlyers is MANY and OurShooters is MANY then Threat is MEDIUM");
		addRule("if CastleWalls is MEDIUM and OurShooters is MANY and EnemyWalkers is MANY then Threat is LOW");

	}
	catch(fl::Exception & pe)
	{
		logAi->error("initTacticalAdvantage: %s", pe.getWhat());
	}
	configure();
}

float TacticalAdvantageEngine::getTacticalAdvantage(const CArmedInstance * we, const CArmedInstance * enemy)
{
	float output = 1;
	try
	{
		armyStructure ourStructure = evaluateArmyStructure(we);
		armyStructure enemyStructure = evaluateArmyStructure(enemy);

		ourWalkers->setValue(ourStructure.walkers);
		ourShooters->setValue(ourStructure.shooters);
		ourFlyers->setValue(ourStructure.flyers);
		ourSpeed->setValue(ourStructure.maxSpeed);

		enemyWalkers->setValue(enemyStructure.walkers);
		enemyShooters->setValue(enemyStructure.shooters);
		enemyFlyers->setValue(enemyStructure.flyers);
		enemySpeed->setValue(enemyStructure.maxSpeed);

		bool bank = dynamic_cast<const CBank *>(enemy);
		if(bank)
			bankPresent->setValue(1);
		else
			bankPresent->setValue(0);

		const CGTownInstance * fort = dynamic_cast<const CGTownInstance *>(enemy);
		if(fort)
			castleWalls->setValue(fort->fortLevel());
		else
			castleWalls->setValue(0);

		engine.process();
		output = threat->getValue();
	}
	catch(fl::Exception & fe)
	{
		logAi->error("getTacticalAdvantage: %s ", fe.getWhat());
	}

	if(output < 0 || (output != output))
	{
		fl::scalar tab[] = { bankPresent->getValue(), castleWalls->getValue(), ourWalkers->getValue(), ourShooters->getValue(), ourFlyers->getValue(), ourSpeed->getValue(),
			enemyWalkers->getValue(), enemyShooters->getValue(), enemyFlyers->getValue(), enemySpeed->getValue() };
		std::string names[] = { "bankPresent", "castleWalls", "ourWalkers", "ourShooters", "ourFlyers", "ourSpeed", "enemyWalkers", "enemyShooters", "enemyFlyers", "enemySpeed" };
		std::stringstream log("Warning! Fuzzy engine doesn't cover this set of parameters: ");

		for(int i = 0; i < boost::size(tab); i++)
			log << names[i] << ": " << tab[i] << " ";
		logAi->error(log.str());
		assert(false);
	}

	return output;
}

//std::shared_ptr<AbstractGoal> chooseSolution (std::vector<std::shared_ptr<AbstractGoal>> & vec)

Goals::TSubgoal FuzzyHelper::chooseSolution(Goals::TGoalVec vec)
{
	if(vec.empty()) //no possibilities found
		return sptr(Goals::Invalid());

	ai->cachedSectorMaps.clear();

	//a trick to switch between heroes less often - calculatePaths is costly
	auto sortByHeroes = [](const Goals::TSubgoal & lhs, const Goals::TSubgoal & rhs) -> bool
	{
		return lhs->hero.h < rhs->hero.h;
	};
	boost::sort(vec, sortByHeroes);

	for(auto g : vec)
	{
		setPriority(g);
	}

	auto compareGoals = [](const Goals::TSubgoal & lhs, const Goals::TSubgoal & rhs) -> bool
	{
		return lhs->priority < rhs->priority;
	};
	return *boost::max_element(vec, compareGoals);
}

float FuzzyHelper::evaluate(Goals::Explore & g)
{
	return 1;
}
float FuzzyHelper::evaluate(Goals::RecruitHero & g)
{
	return 1;
}
HeroMovementGoalEngineBase::HeroMovementGoalEngineBase()
{
	try
	{
		strengthRatio = vstd::make_unique<fl::InputVariable>(fl::InputVariable("strengthRatio")); //hero must be strong enough to defeat guards
		heroStrength = vstd::make_unique<fl::InputVariable>(fl::InputVariable("heroStrength")); //we want to use weakest possible hero
		turnDistance = vstd::make_unique<fl::InputVariable>(fl::InputVariable("turnDistance")); //we want to use hero who is near
		missionImportance = vstd::make_unique<fl::InputVariable>(fl::InputVariable("lockedMissionImportance")); //we may want to preempt hero with low-priority mission
		value = vstd::make_unique<fl::OutputVariable>(fl::OutputVariable("Value"));
		value->setMinimum(0);
		value->setMaximum(5);

		for(auto val : { strengthRatio.get(), heroStrength.get(), turnDistance.get(), missionImportance.get() })
		{
			engine.addInputVariable(val);
		}
		engine.addOutputVariable(value.get());

		strengthRatio->addTerm(new fl::Ramp("LOW", SAFE_ATTACK_CONSTANT, 0));
		strengthRatio->addTerm(new fl::Ramp("HIGH", SAFE_ATTACK_CONSTANT, SAFE_ATTACK_CONSTANT * 3));
		strengthRatio->setRange(0, SAFE_ATTACK_CONSTANT * 3);

		//strength compared to our main hero
		heroStrength->addTerm(new fl::Ramp("LOW", 0.2, 0));
		heroStrength->addTerm(new fl::Triangle("MEDIUM", 0.2, 0.8));
		heroStrength->addTerm(new fl::Ramp("HIGH", 0.5, 1));
		heroStrength->setRange(0.0, 1.0);

		turnDistance->addTerm(new fl::Ramp("SMALL", 0.5, 0));
		turnDistance->addTerm(new fl::Triangle("MEDIUM", 0.1, 0.8));
		turnDistance->addTerm(new fl::Ramp("LONG", 0.5, 3));
		turnDistance->setRange(0.0, 3.0);

		missionImportance->addTerm(new fl::Ramp("LOW", 2.5, 0));
		missionImportance->addTerm(new fl::Triangle("MEDIUM", 2, 3));
		missionImportance->addTerm(new fl::Ramp("HIGH", 2.5, 5));
		missionImportance->setRange(0.0, 5.0);

		//an issue: in 99% cases this outputs center of mass (2.5) regardless of actual input :/
		//should be same as "mission Importance" to keep consistency
		value->addTerm(new fl::Ramp("LOW", 2.5, 0));
		value->addTerm(new fl::Triangle("MEDIUM", 2, 3)); //can't be center of mass :/
		value->addTerm(new fl::Ramp("HIGH", 2.5, 5));
		value->setRange(0.0, 5.0);

		//use unarmed scouts if possible
		addRule("if strengthRatio is HIGH and heroStrength is LOW then Value is very HIGH");
		//we may want to use secondary hero(es) rather than main hero
		addRule("if strengthRatio is HIGH and heroStrength is MEDIUM then Value is somewhat HIGH");
		addRule("if strengthRatio is HIGH and heroStrength is HIGH then Value is somewhat LOW");
		//don't assign targets to heroes who are too weak, but prefer targets of our main hero (in case we need to gather army)
		addRule("if strengthRatio is LOW and heroStrength is LOW then Value is very LOW");
		//attempt to arm secondary heroes is not stupid
		addRule("if strengthRatio is LOW and heroStrength is MEDIUM then Value is somewhat HIGH");
		addRule("if strengthRatio is LOW and heroStrength is HIGH then Value is LOW");

		//do not cancel important goals
		addRule("if lockedMissionImportance is HIGH then Value is very LOW");
		addRule("if lockedMissionImportance is MEDIUM then Value is somewhat LOW");
		addRule("if lockedMissionImportance is LOW then Value is HIGH");
		//pick nearby objects if it's easy, avoid long walks
		addRule("if turnDistance is SMALL then Value is HIGH");
		addRule("if turnDistance is MEDIUM then Value is MEDIUM");
		addRule("if turnDistance is LONG then Value is LOW");
	}
	catch(fl::Exception & fe)
	{
		logAi->error("HeroMovementGoalEngineBase: %s", fe.getWhat());
	}
}

void HeroMovementGoalEngineBase::setSharedFuzzyVariables(Goals::AbstractGoal & goal)
{
	float turns = calculateTurnDistanceInputValue(goal.hero.h, goal.tile);
	float missionImportanceData = 0;
	if(vstd::contains(ai->lockedHeroes, goal.hero))
		missionImportanceData = ai->lockedHeroes[goal.hero]->priority;

	float strengthRatioData = 10.0f; //we are much stronger than enemy
	ui64 danger = evaluateDanger(goal.tile, goal.hero.h);
	if(danger)
		strengthRatioData = (fl::scalar)goal.hero.h->getTotalStrength() / danger;

	try
	{
		strengthRatio->setValue(strengthRatioData);
		heroStrength->setValue((fl::scalar)goal.hero->getTotalStrength() / ai->primaryHero()->getTotalStrength());
		turnDistance->setValue(turns);
		missionImportance->setValue(missionImportanceData);
	}
	catch(fl::Exception & fe)
	{
		logAi->error("HeroMovementGoalEngineBase::setSharedFuzzyVariables: %s", fe.getWhat());
	}
}

float FuzzyHelper::evaluate(Goals::VisitTile & g)
{
	return visitTileEngine.evaluate(g);
}
float FuzzyHelper::evaluate(Goals::GetObj & g)
{
	return getObjEngine.evaluate(g);
}
float FuzzyHelper::evaluate(Goals::VisitHero & g)
{
	auto obj = cb->getObj(ObjectInstanceID(g.objid)); //we assume for now that these goals are similar
	if(!obj)
		return -100; //hero died in the meantime
	//TODO: consider direct copy (constructor?)
	g.setpriority(Goals::VisitTile(obj->visitablePos()).sethero(g.hero).setisAbstract(g.isAbstract).accept(this));
	return g.priority;
}
float FuzzyHelper::evaluate(Goals::GatherArmy & g)
{
	//the more army we need, the more important goal
	//the more army we lack, the less important goal
	float army = g.hero->getArmyStrength();
	float ratio = g.value / std::max(g.value - army, 2000.0f); //2000 is about the value of hero recruited from tavern
	return 5 * (ratio / (ratio + 2)); //so 50% army gives 2.5, asymptotic 5
}

float FuzzyHelper::evaluate(Goals::ClearWayTo & g)
{
	if(!g.hero.h)
		throw cannotFulfillGoalException("ClearWayTo called without hero!");

	int3 t = ai->getCachedSectorMap(g.hero)->firstTileToGet(g.hero, g.tile);

	if(t.valid())
	{
		if(isSafeToVisit(g.hero, t))
		{
			g.setpriority(Goals::VisitTile(g.tile).sethero(g.hero).setisAbstract(g.isAbstract).accept(this));
		}
		else
		{
			g.setpriority (Goals::GatherArmy(evaluateDanger(t, g.hero.h)*SAFE_ATTACK_CONSTANT).
				sethero(g.hero).setisAbstract(true).accept(this));
		}
		return g.priority;
	}
	else
		return -1;

}

float FuzzyHelper::evaluate(Goals::BuildThis & g)
{
	return g.priority; //TODO
}
float FuzzyHelper::evaluate(Goals::DigAtTile & g)
{
	return 0;
}
float FuzzyHelper::evaluate(Goals::CollectRes & g)
{
	return g.priority; //handled by ResourceManager
}
float FuzzyHelper::evaluate(Goals::Build & g)
{
	return 0;
}
float FuzzyHelper::evaluate(Goals::BuyArmy & g)
{
	return g.priority;
}
float FuzzyHelper::evaluate(Goals::Invalid & g)
{
	return -1e10;
}
float FuzzyHelper::evaluate(Goals::AbstractGoal & g)
{
	logAi->warn("Cannot evaluate goal %s", g.name());
	return g.priority;
}
void FuzzyHelper::setPriority(Goals::TSubgoal & g) //calls evaluate - Visitor pattern
{
	g->setpriority(g->accept(this)); //this enforces returned value is set
}

GetObjEngine::GetObjEngine()
{
	try
	{
		objectValue = vstd::make_unique<fl::InputVariable>(fl::InputVariable("objectValue")); //value of that object type known by AI
		
		engine.addInputVariable(objectValue.get());

		//objectValue ranges are based on checking RMG priorities of some objects and trying to guess sane value ranges
		objectValue->addTerm(new fl::Ramp("LOW", 3000, 0)); //I have feeling that concave shape might work well instead of ramp for objectValue FL terms
		objectValue->addTerm(new fl::Triangle("MEDIUM", 2500, 6000));
		objectValue->addTerm(new fl::Ramp("HIGH", 5000, 20000));
		objectValue->setRange(0, 20000); //relic artifact value is border value by design, even better things are scaled down.

		addRule("if turnDistance is LONG and objectValue is HIGH then value is MEDIUM");
		addRule("if turnDistance is MEDIUM and objectValue is HIGH then value is somewhat HIGH");
		addRule("if turnDistance is SHORT and objectValue is HIGH then value is HIGH");

		addRule("if turnDistance is LONG and objectValue is MEDIUM then value is somewhat LOW");
		addRule("if turnDistance is MEDIUM and objectValue is MEDIUM then value is MEDIUM");
		addRule("if turnDistance is SHORT and objectValue is MEDIUM then value is somewhat HIGH");

		addRule("if turnDistance is LONG and objectValue is LOW then value is very LOW");
		addRule("if turnDistance is MEDIUM and objectValue is LOW then value is LOW");
		addRule("if turnDistance is SHORT and objectValue is LOW then value is MEDIUM");
	}
	catch(fl::Exception & fe)
	{
		logAi->error("FindWanderTarget: %s", fe.getWhat());
	}
	configure();
}

float GetObjEngine::evaluate(Goals::AbstractGoal & goal)
{
	auto g = dynamic_cast<Goals::GetObj &>(goal);

	if(!g.hero)
		return 0;

	auto obj = cb->getObj(ObjectInstanceID(g.objid));


	boost::optional<int> objValueKnownByAI = MapObjectsEvaluator::getInstance().getObjectValue(obj->ID, obj->subID);
	int objValue = 0;

	if(objValueKnownByAI != boost::none) //consider adding value manipulation based on object instances on map
	{
		objValue = std::min(std::max(objValueKnownByAI.get(), 0), 20000);
	}
	else
	{
		MapObjectsEvaluator::getInstance().addObjectData(obj->ID, obj->subID, 0);
		logGlobal->warn("AI met object type it doesn't know - ID: " + std::to_string(obj->ID) + ", subID: " + std::to_string(obj->subID) + " - adding to database with value " + std::to_string(objValue));
	}

	setSharedFuzzyVariables(goal);

	float output = -1.0f;
	try
	{
		objectValue->setValue(objValue);
		engine.process();
		output = value->getValue();
	}
	catch(fl::Exception & fe)
	{
		logAi->error("evaluate getWanderTargetObjectValue: %s", fe.getWhat());
	}
	assert(output >= 0.0f);
	return output;
}

VisitTileEngine::VisitTileEngine() //so far no VisitTile-specific variables that are not shared with HeroMovementGoalEngineBase
{
	configure();
}

float VisitTileEngine::evaluate(Goals::AbstractGoal & goal)
{
	auto g = dynamic_cast<Goals::VisitTile &>(goal);
	//we assume that hero is already set and we want to choose most suitable one for the mission
	if(!g.hero)
		return 0;

	//assert(cb->isInTheMap(g.tile));

	setSharedFuzzyVariables(goal);

	try
	{
		engine.process();
		g.priority = value->getValue();
	}
	catch(fl::Exception & fe)
	{
		logAi->error("evaluate VisitTile: %s", fe.getWhat());
	}
	assert(g.priority >= 0);
	return g.priority;
}
