/***********************************************************************************
Copyright (c) 2017, Michael Neunert, Markus Giftthaler, Markus Stäuble, Diego Pardo,
Farbod Farshidian. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of ETH ZURICH nor the names of its contributors may be used
      to endorse or promote products derived from this software without specific
      prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL ETH ZURICH BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
***************************************************************************************/


#ifndef INCLUDE_CT_OPTCON_NLOC_BACKEND_BASE_HPP_
#define INCLUDE_CT_OPTCON_NLOC_BACKEND_BASE_HPP_

#include <atomic>

#include <ct/core/core.h>
#include <ct/optcon/costfunction/CostFunctionQuadratic.hpp>
#include <ct/optcon/solver/OptConSolver.h>

#include <ct/optcon/problem/LQOCProblem.hpp>
#include <ct/optcon/solver/lqp/GNRiccatiSolver.hpp>
#include <ct/optcon/solver/lqp/HPIPMInterface.hpp>
#include <ct/optcon/solver/NLOptConSettings.hpp>

#include "NLOCResults.hpp"

#ifdef MATLAB
#include <ct/optcon/matlab.hpp>
#endif

#define SYMPLECTIC_ENABLED template<size_t V, size_t P> typename std::enable_if<(V > 0 && P > 0), void>::type
#define SYMPLECTIC_DISABLED template<size_t V, size_t P> typename std::enable_if<(V <= 0 || P <= 0), void>::type

namespace ct{
namespace optcon{


/*!
 * \ingroup GNMS
 *
 * \brief C++ implementation of GNMS.
 *
 *  The implementation and naming is based on the reference below. In general, the code follows this convention:
 *  X  <- Matrix (upper-case in paper)
 *  xv <- vector (lower-case bold in paper)
 *  x  <- scalar (lower-case in paper)
 *
 */

template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR = double>
class NLOCBackendBase
{

public:

	EIGEN_MAKE_ALIGNED_OPERATOR_NEW

	static const size_t state_dim = STATE_DIM;
	static const size_t control_dim = CONTROL_DIM;

	typedef NLOptConSettings Settings_t;
	typedef core::StateFeedbackController<STATE_DIM, CONTROL_DIM, SCALAR> Policy_t;

	typedef OptConProblem<STATE_DIM, CONTROL_DIM, SCALAR> OptConProblem_t;

	typedef OptConSolver<NLOCBackendBase, Policy_t, Settings_t, STATE_DIM, CONTROL_DIM, SCALAR> Base;

	typedef LQOCProblem<STATE_DIM, CONTROL_DIM, SCALAR> LQOCProblem_t;
	typedef LQOCSolver<STATE_DIM, CONTROL_DIM, SCALAR>  LQOCSolver_t;

	typedef core::LinearSystemDiscretizer<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR> LinearSystemDiscretizer_t;

	typedef ct::core::StateVectorArray<STATE_DIM, SCALAR> StateVectorArray;
	typedef ct::core::ControlVectorArray<CONTROL_DIM, SCALAR> ControlVectorArray;

	typedef ct::core::ControlMatrix<CONTROL_DIM, SCALAR> ControlMatrix;
	typedef ct::core::ControlMatrixArray<CONTROL_DIM, SCALAR> ControlMatrixArray;
	typedef ct::core::StateMatrixArray<STATE_DIM, SCALAR> StateMatrixArray;
	typedef ct::core::StateControlMatrixArray<STATE_DIM, CONTROL_DIM, SCALAR> StateControlMatrixArray;
	typedef ct::core::FeedbackArray<STATE_DIM, CONTROL_DIM, SCALAR> FeedbackArray;
	typedef ct::core::tpl::TimeArray<SCALAR> TimeArray;

	typedef Eigen::Matrix<SCALAR, STATE_DIM, STATE_DIM> state_matrix_t;
	typedef Eigen::Matrix<SCALAR, CONTROL_DIM, CONTROL_DIM> control_matrix_t;
	typedef Eigen::Matrix<SCALAR, CONTROL_DIM, STATE_DIM> control_state_matrix_t;
	typedef Eigen::Matrix<SCALAR, STATE_DIM, CONTROL_DIM> state_control_matrix_t;

	typedef core::StateVector<STATE_DIM, SCALAR> state_vector_t;
	typedef core::ControlVector<CONTROL_DIM, SCALAR> control_vector_t;
	typedef core::FeedbackMatrix<STATE_DIM, CONTROL_DIM, SCALAR> feedback_matrix_t;


	typedef SCALAR scalar_t;
	typedef std::vector<SCALAR, Eigen::aligned_allocator<SCALAR>> scalar_array_t;



	NLOCBackendBase(const OptConProblem<STATE_DIM, CONTROL_DIM, SCALAR>& optConProblem,
			const Settings_t& settings) :

		    integrators_(settings.nThreads+1),
			integratorsEulerSymplectic_(settings.nThreads+1),
			integratorsRkSymplectic_(settings.nThreads+1),

		    controller_(settings.nThreads+1),
		    settings_(settings),
			initialized_(false),
			configured_(false),
			iteration_(0),
			K_(0),
			d_norm_(0.0),
			lx_norm_(0.0),
			lu_norm_(0.0),
		    lqocProblem_(new LQOCProblem<STATE_DIM, CONTROL_DIM, SCALAR>()),
			intermediateCostBest_(std::numeric_limits<SCALAR>::infinity()),
			finalCostBest_(std::numeric_limits<SCALAR>::infinity()),
			lowestCost_(std::numeric_limits<SCALAR>::infinity()),
			intermediateCostPrevious_(std::numeric_limits<SCALAR>::infinity()),
			finalCostPrevious_(std::numeric_limits<SCALAR>::infinity()),
			linearSystemDiscretizers_(settings.nThreads+1, LinearSystemDiscretizer_t(settings.dt)),
			firstRollout_(true),
			alphaBest_(-1)
	{
		Eigen::initParallel();

		for (int i=0; i<settings.nThreads+1; i++)
		{
			controller_[i] = ConstantControllerPtr (new core::ConstantController<STATE_DIM, CONTROL_DIM, SCALAR>());
		}

		configure(settings);

		changeTimeHorizon(optConProblem.getTimeHorizon());
		changeInitialState(optConProblem.getInitialState());
		changeCostFunction(optConProblem.getCostFunction());
		changeNonlinearSystem(optConProblem.getNonlinearSystem());
		changeLinearSystem(optConProblem.getLinearSystem());

		// to be included later
//		if(optConProblem.getStateInputConstraints())
//			changeStateInputConstraints(optConProblem.getStateInputConstraints());
//		if(optConProblem.getPureStateConstraints())
//			changePureStateConstraints(optConProblem.getPureStateConstraints());
	}


	NLOCBackendBase(const OptConProblem<STATE_DIM, CONTROL_DIM, SCALAR>& optConProblem,
			 const std::string& settingsFile,
			 bool verbose = true,
			 const std::string& ns = "alg") :
			NLOCBackendBase(optConProblem, Settings_t::fromConfigFile(settingsFile, verbose, ns))
	{}


	virtual ~NLOCBackendBase() {};


	//! configure the solver
	/**
	 * Configure the solver
	 * @param settings solver settings
	 */
	virtual void configure(const Settings_t& settings);


	//! get the current SLQsolver settings
	const Settings_t& getSettings() const { return settings_; }


	/*!
	 * Set the initial guess used by the solver (not all solvers might support initial guesses)
	 */
	void setInitialGuess(const Policy_t& initialGuess);


	/*!
	 * \brief Change the time horizon the solver operates on.
	 *
	 * This function does not need to be called if setOptConProblem() has been called
	 * with an OptConProblem that had the correct time horizon set.
	 */
	void changeTimeHorizon(const SCALAR& tf);


	SCALAR getTimeHorizon() {return K_* settings_.dt ;}

	int getNumSteps() {return K_;}
	int getNumStepsPerShot() {return settings_.K_shot;}

	SYMPLECTIC_ENABLED initializeSymplecticIntegrators(size_t i);
	SYMPLECTIC_DISABLED initializeSymplecticIntegrators(size_t i) {};

	SYMPLECTIC_ENABLED integrateSymplectic(size_t threadId, ct::core::StateVector<STATE_DIM, SCALAR>& x0, const double& t, const size_t& steps, const double& dt_sim) const;
	SYMPLECTIC_DISABLED integrateSymplectic(size_t threadId, ct::core::StateVector<STATE_DIM, SCALAR>& x0, const double& t, const size_t& steps, const double& dt_sim) const
	{
		throw std::runtime_error("Symplectic integrator selected but invalid dimensions for it. Check V_DIM>1, P_DIM>1");
	}


	/*!
	 * \brief Change the initial state for the optimal control problem
	 *
	 * This function does not need to be called if setOptConProblem() has been called
	 * with an OptConProblem that had the correct initial state set
	 */
	void changeInitialState(const core::StateVector<STATE_DIM, SCALAR>& x0);

	/*!
	 * \brief Change the cost function
	 *
	 * This function does not need to be called if setOptConProblem() has been called
	 * with an OptConProblem that had the correct cost function
	 */
	void changeCostFunction(const typename Base::OptConProblem_t::CostFunctionPtr_t& cf);

	/*!
	 * \brief Change the nonlinear system
	 *
	 * This function does not need to be called if setOptConProblem() has been called
	 * with an OptConProblem that had the correct nonlinear system
	 */
	void changeNonlinearSystem(const typename Base::OptConProblem_t::DynamicsPtr_t& dyn);

	/*!
	 * \brief Change the linear system
	 *
	 * This function does not need to be called if setOptConProblem() has been called
	 * with an OptConProblem that had the correct linear system
	 */
	void changeLinearSystem(const typename Base::OptConProblem_t::LinearPtr_t& lin);


	/*!
	 * \brief Direct accessor to the system instances
	 *
	 * \warning{Use this only when performance absolutely matters and if you know what you
	 * are doing. Otherwise use e.g. changeNonlinearSystem() to change the system dynamics
	 * in a safe and easy way. You should especially not change the size of the vector or
	 * modify each entry differently.}
	 * @return
	 */
	std::vector<typename OptConProblem_t::DynamicsPtr_t>& getNonlinearSystemsInstances() { return systems_; }

	const std::vector<typename OptConProblem_t::DynamicsPtr_t>& getNonlinearSystemsInstances() const { return systems_; }

	/*!
	 * \brief Direct accessor to the linear system instances
	 *
	 * \warning{Use this only when performance absolutely matters and if you know what you
	 * are doing. Otherwise use e.g. changeLinearSystem() to change the system dynamics
	 * in a safe and easy way. You should especially not change the size of the vector or
	 * modify each entry differently.}
	 * @return
	 */
	std::vector<typename OptConProblem_t::LinearPtr_t>& getLinearSystemsInstances() { return linearSystems_; }

	const std::vector<typename OptConProblem_t::LinearPtr_t>& getLinearSystemsInstances() const { return linearSystems_; }

	/*!
	 * \brief Direct accessor to the cost function instances
	 *
	 * \warning{Use this only when performance absolutely matters and if you know what you
	 * are doing. Otherwise use e.g. changeCostFunction() to change the system dynamics
	 * in a safe and easy way. You should especially not change the size of the vector or
	 * modify each entry differently.}
	 * @return
	 */
	std::vector<typename OptConProblem_t::CostFunctionPtr_t>& getCostFunctionInstances() { return costFunctions_; }

	const std::vector<typename OptConProblem_t::CostFunctionPtr_t>& getCostFunctionInstances() const { return costFunctions_; }

	/**
	 * @brief      Direct accessor to the state input constraint instances
	 *
	 * \warning{Use this only when performance absolutely matters and if you know what you
	 * are doing. Otherwise use e.g. changeCostFunction() to change the system dynamics
	 * in a safe and easy way. You should especially not change the size of the vector or
	 * modify each entry differently.}
	 *
	 * @return     The state input constraint instances
	 */
	std::vector<typename OptConProblem_t::ConstraintPtr_t>& getStateInputConstraintsInstances() { return stateInputConstraints_; }

	const std::vector<typename OptConProblem_t::ConstraintPtr_t>& getStateInputConstraintsInstances() const { return stateInputConstraints_; }

	/**
	 * @brief      Direct accessor to the pure state constraints
	 *
	 * \warning{Use this only when performance absolutely matters and if you know what you
	 * are doing. Otherwise use e.g. changeCostFunction() to change the system dynamics
	 * in a safe and easy way. You should especially not change the size of the vector or
	 * modify each entry differently.}
	 *
	 * @return     The pure state constraints instances.
	 */
	std::vector<typename OptConProblem_t::ConstraintPtr_t>& getPureStateConstraintsInstances() { return pureStateConstraints_; }

	const std::vector<typename OptConProblem_t::ConstraintPtr_t>& getPureStateConstraintsInstances() const { return pureStateConstraints_; }


	/*!
	 * Tests consistency of the instance of the dynamics, linear systems and costs. This is not a test for thread safety.
	 * @return returns true if instances are consistent with each other
	 */
	bool testConsistency()
	{
		return true;
	}


	//! Export all functions to matlab workspace
	/*!
	  This function can be used for Debugging. It exports all variables to Matlab after each iteration. It also saves
	  the Matlab workspace to a .mat file.
	*/
	void logToMatlab(const size_t& iteration);

	//! log the initial guess to Matlab
	void logInitToMatlab();

	//! return the cost of the solution of the current iteration
	SCALAR getCost() const;

	//! return the sum of the L2-norm of the defects along the solution candidate
	SCALAR getTotalDefect() const { return d_norm_;}

	void reset();

	const core::StateTrajectory<STATE_DIM, SCALAR> getStateTrajectory() const;

	const core::ControlTrajectory<CONTROL_DIM, SCALAR> getControlTrajectory() const;


	const Policy_t& getSolution();

	const TimeArray& getTimeArray() {return t_;}

	bool isConfigured() {return configured_;}

	bool isInitialized() {return initialized_;}


	//! Retrieve Last Linearized Model
	/*!
	  Retrieve the linearized model computed during the last iteration
	*/
	void retrieveLastLinearizedModel(StateMatrixArray& A, StateControlMatrixArray& B);

	/*!
	 * the prepare Solve LQP Problem method is intended for a special use-case: unconstrained GNMS with pre-solving of the
	 */
	virtual void prepareSolveLQProblem(size_t startIndex);


	virtual void finishSolveLQProblem(size_t endIndex);

	/*!
	 * solve Full LQProblem, e.g. to be used with HPIPM or if we have a constrained problem
	 */
	virtual void solveFullLQProblem();

	//! compute costs of solution candidate
	void updateCosts();

	//! nominal rollout using default thread and member variables for the results. // todo maybe rename (initial rollout?)
	bool nominalRollout() {

		StateVectorArray x_lqr_ref = x_;
		ControlVectorArray u_recorded;

		bool success = simpleRollout(
				settings_.nThreads,
				u_ff_,
				x_lqr_ref, x_,u_recorded
				);
		u_ff_ = u_recorded;

//		bool success =  rolloutSingleShot(settings_.nThreads, 0, u_ff_, x_, x_, xShot_);


		x_prev_ = x_;
		u_ff_prev_ = u_ff_;
		firstRollout_ = false;
		return success;
	}

	//! check problem for consistency
	void checkProblem();

	//! return the current iteration number
	size_t& iteration() {return iteration_;}

	//! Print iteration summary
	/*!
	 *  This function is automatically called if the printSummary settings is on. It prints out important information
	 *  like cost etc. after each iteration.
	 */
	void printSummary();

	//! perform line-search and update controller for single shooting
	bool lineSearchSingleShooting();

	//! perform line-search and update controller for multiple shooting
	bool lineSearchMultipleShooting();

	//! Computes the linearization of the dynamics along the trajectory, for the specified indices. See computeLinearizedDynamics for details
	virtual void computeLinearizedDynamicsAroundTrajectory(size_t firstIndex, size_t lastIndex) = 0;

	//! Computes the quadratic approximation of the cost function along the trajectory, for the specified indices
	virtual void computeQuadraticCostsAroundTrajectory(size_t firstIndex, size_t lastIndex) = 0;

	//! obtain state update from lqoc solver
	void getStateUpdates() {lx_ = lqocSolver_->getStateUpdates();}

	//! obtain control update from lqoc solver
	void getControlUpdates() {lu_ = lqocSolver_->getControlUpdates();}

	//! obtain feedback update from lqoc solver, if provided
	void getFeedback()
	{
		if(settings_.closedLoopShooting)
			L_ = lqocSolver_->getFeedback();
		else
			L_.setConstant(core::FeedbackMatrix<STATE_DIM, CONTROL_DIM, SCALAR>::Zero()); // todo can eventually go away to save time
	}

	//! reset all defects to zero
	void resetDefects() {lqocProblem_->b_.setConstant(state_vector_t::Zero());}

	//! update the nominal defects
	void computeDefectsNorm() {d_norm_ = computeDefectsNorm<1>(lqocProblem_->b_);}

	//! integrates the specified shots and computes the corresponding defects
	virtual void rolloutShots(size_t firstIndex, size_t lastIndex) = 0;

	//! do a single threaded rollout and defect computation of the shots - useful for line-search
	void rolloutShotsSingleThreaded(size_t threadId,
			size_t firstIndex,
			size_t lastIndex,
			ControlVectorArray& u_ff_local,
			StateVectorArray& x_local,
			const StateVectorArray& x_ref_lqr,
			StateVectorArray& xShot,
			StateVectorArray& d) const;

	//! performLineSearch: execute the line search, possibly with different threading schemes
	virtual SCALAR performLineSearch() = 0;


	//! simple full-step update for state and feedforward control (used for MPC-mode!)
	void doFullStepUpdate();

	void logSummaryToMatlab(const std::string& fileName) {summaryAllIterations_.logToMatlab(fileName);}

protected:

	//! integrate the individual shots
	bool rolloutSingleShot(
			const size_t threadId,
			const size_t k,
			ControlVectorArray& u_ff_local,
			StateVectorArray& x_local,
			const StateVectorArray& x_ref_lqr,
			StateVectorArray& xShot,
			std::atomic_bool* terminationFlag = nullptr ) const;

	bool simpleRollout(
			const size_t threadId,
			const ControlVectorArray& uff,
			const StateVectorArray& x_ref_lqr,
			StateVectorArray& x_local,
			ControlVectorArray& u_recorded
			)const;

	//! computes the defect between shot and trajectory
	/*!
	 * @param k			index of the shot under consideration
	 * @param x_local	the state trajectory
	 * @param xShot		the shot trajectory
	 * @param d			the defect trajectory
	 */
	void computeSingleDefect(
			size_t k,
			const StateVectorArray& x_local,
			const StateVectorArray& xShot,
			StateVectorArray& d) const;

	//! Computes the linearized Dynamics at a specific point of the trajectory
	/*!
	  This function calculates the linearization, i.e. matrices A and B in \f$ \dot{x} = A(x(k)) x + B(x(k)) u \f$
	  at a specific point of the trajectory

	  \param threadId the id of the worker thread
	  \param k step k
	*/
	void computeLinearizedDynamics(size_t threadId, size_t k);

	//! Computes the quadratic costs
	/*!
	  This function calculates the quadratic costs as provided by the costFunction pointer.

	 * \param threadId id of worker thread
	 * \param k step k
	*/
	void computeQuadraticCosts(size_t threadId, size_t k);

	//! Initializes cost to go
	/*!
	 * This function initializes the cost-to-go function at time K.
     *
	 */
	void initializeCostToGo();

	//! Computes cost to go
	/*!
	 * This function computes the cost-to-go function for all times t<t_K
     *
	 * \param k step k
	 */
	void computeCostToGo(size_t k);

	//! Design controller
	/*!
	 * This function designes the LQR and feedforward controller at time k.
     *
	 * \param k step k
	 */
	void designController(size_t k);

	//! Compute cost for a given set of state and input trajectory
	/*!
	 * Compute cost for a given set of state and input trajectory
     *
	 * \param threadId the ID of the thread
	 * \param x_local the state trajectory
	 * \param u_local the control trajectory
	 * \param intermediateCost the accumulated intermediate cost
	 * \param finalCost the accumulated final cost
	 */
	void computeCostsOfTrajectory(
			size_t threadId,
			const core::StateVectorArray<STATE_DIM, SCALAR>& x_local,
			const core::ControlVectorArray<CONTROL_DIM, SCALAR>& u_local,
			scalar_t& intermediateCost,
			scalar_t& finalCost
	) const;


	//! Check if controller with particular alpha is better
	void executeLineSearchSingleShooting(
			const size_t threadId,
			const scalar_t alpha,
			StateVectorArray& x_local,
			ControlVectorArray& u_local,
			scalar_t& intermediateCost,
			scalar_t& finalCost,
			std::atomic_bool* terminationFlag = nullptr
	) const;


	void executeLineSearchMultipleShooting(
			const size_t threadId,
			const scalar_t alpha,
			const ControlVectorArray& u_ff_update,
			const StateVectorArray& x_update,
			ct::core::StateVectorArray<STATE_DIM, SCALAR>& x_recorded,
			ct::core::StateVectorArray<STATE_DIM, SCALAR>& x_shot_recorded,
			ct::core::StateVectorArray<STATE_DIM, SCALAR>& defects_recorded,
			ct::core::ControlVectorArray<CONTROL_DIM, SCALAR>& u_recorded,
			scalar_t& intermediateCost,
			scalar_t& finalCost,
			scalar_t& defectNorm,
			std::atomic_bool* terminationFlag = nullptr
	) const;


	//! Update feedforward controller
	/*!
	 * This function updates the feedforward Controller based on the previous calculation.
     *
	 * \param k step k
	 */
	void updateFFController(size_t k);


	//! Send a std::vector of Eigen to Matlab
	/*!
	 * This is a helper function to efficiently send std::vectors to Matlab.
	 */
	template<class V>
	void matrixToMatlab(V& matrix, std::string variableName);

	//! compute norm of a discrete array (todo move to core)
	template<typename ARRAY_TYPE, size_t ORDER = 1>
	SCALAR computeDiscreteArrayNorm(const ARRAY_TYPE& d) const;

	//! compute norm of difference between two discrete arrays (todo move to core)
	template<typename ARRAY_TYPE, size_t ORDER = 1>
	SCALAR computeDiscreteArrayNorm(const ARRAY_TYPE& a, const ARRAY_TYPE& b) const;

	//! compute the norm of the defects trajectory
	/*!
	 * Note that different kind of norms might be favorable for different cases.
	 * According to Nocedal and Wright, the l1-norm is "exact" (p.435),  the l2-norm is smooth.
	 */
	template<size_t ORDER = 1>
	SCALAR computeDefectsNorm(const StateVectorArray& d) const { return computeDiscreteArrayNorm<StateVectorArray, ORDER>(d);}

	typedef std::shared_ptr<ct::core::Integrator<STATE_DIM, SCALAR> > IntegratorPtr;
    std::vector<IntegratorPtr, Eigen::aligned_allocator<IntegratorPtr> > integrators_; //! Runge-Kutta-4 Integrators

	typedef std::shared_ptr<ct::core::IntegratorSymplecticEuler<P_DIM, V_DIM, CONTROL_DIM, SCALAR> > IntegratorSymplecticEulerPtr;
	std::vector<IntegratorSymplecticEulerPtr, Eigen::aligned_allocator<IntegratorSymplecticEulerPtr> > integratorsEulerSymplectic_;

	typedef std::shared_ptr<ct::core::IntegratorSymplecticRk<P_DIM, V_DIM, CONTROL_DIM, SCALAR> > IntegratorSymplecticRkPtr;
	std::vector<IntegratorSymplecticRkPtr, Eigen::aligned_allocator<IntegratorSymplecticRkPtr > > integratorsRkSymplectic_;

    typedef std::shared_ptr<core::ConstantController<STATE_DIM, CONTROL_DIM, SCALAR> > ConstantControllerPtr;
    std::vector<ConstantControllerPtr, Eigen::aligned_allocator<ConstantControllerPtr> > controller_;	//! the constant controller for forward-integration during one time-step



    //! The policy. currently only for returning the result, should eventually replace L_ and u_ff_ (todo)
    NLOCBackendBase::Policy_t policy_;

    ct::core::tpl::TimeArray<SCALAR> t_; //! the time trajectory

    bool initialized_;
    bool configured_;

	size_t iteration_;	/*!< current iteration */

	Settings_t settings_;

	int K_; //! the number of stages in the overall OptConProblem

	StateVectorArray lx_;
	StateVectorArray x_;
	StateVectorArray xShot_;
	StateVectorArray x_prev_;

	ControlVectorArray lu_;
	ControlVectorArray u_ff_;
	ControlVectorArray u_ff_prev_;

	FeedbackArray L_;

	SCALAR d_norm_; 	//! sum of the norms of all defects
	SCALAR lx_norm_; 	//! sum of the norms of state update
	SCALAR lu_norm_; 	//! sum of the norms of control update

	//! shared pointer to the linear-quadratic optimal control problem
	std::shared_ptr<LQOCProblem<STATE_DIM, CONTROL_DIM, SCALAR> > lqocProblem_;

	//! shared pointer to the linear-quadratic optimal control solver
	std::shared_ptr<LQOCSolver<STATE_DIM, CONTROL_DIM, SCALAR> > lqocSolver_;


	scalar_t intermediateCostBest_;
	scalar_t finalCostBest_;
	scalar_t lowestCost_;

	//! costs of the previous iteration, required to determine convergence
	scalar_t intermediateCostPrevious_;
	scalar_t finalCostPrevious_;


	//! if building with MATLAB support, include matfile
#ifdef MATLAB
	matlab::MatFile matFile_;
#endif //MATLAB


	/*!
	 * of the following objects, we have nThreads+1 instantiations in form of a vector.
	 * Every instantiation is dedicated to a certain thread in the multi-thread implementation
	 */
	std::vector<typename OptConProblem_t::DynamicsPtr_t> systems_;
	std::vector<LinearSystemDiscretizer_t> linearSystemDiscretizers_;
	std::vector<typename OptConProblem_t::LinearPtr_t> linearSystems_;
	std::vector<typename OptConProblem_t::CostFunctionPtr_t> costFunctions_;
	std::vector<typename OptConProblem_t::ConstraintPtr_t> stateInputConstraints_;
	std::vector<typename OptConProblem_t::ConstraintPtr_t> pureStateConstraints_;


	bool firstRollout_;
	scalar_t alphaBest_;

	SummaryAllIterations<SCALAR> summaryAllIterations_;

};


} // namespace optcon
} // namespace ct

#include "implementation/NLOCBackendBase-impl.hpp"


#undef SYMPLECTIC_ENABLE
#undef SYMPLECTIC_DISABLED

#endif /* INCLUDE_CT_OPTCON_GNMS_GNMSBASE_HPP_ */
