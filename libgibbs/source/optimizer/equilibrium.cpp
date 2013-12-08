/*=============================================================================
	Copyright (c) 2012-2013 Richard Otis

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

// definition for Equilibrium constructor and helper functions

#include "libgibbs/include/libgibbs_pch.hpp"
#include "libgibbs/include/equilibrium.hpp"
#include "libgibbs/include/conditions.hpp"
#include "libgibbs/include/optimizer/opt_Gibbs.hpp"
#include "libgibbs/include/utils/enum_handling.hpp"
#include "libtdb/include/database.hpp"
#include "libtdb/include/structure.hpp"
#include "libtdb/include/exceptions.hpp"
#include "libtdb/include/logging.hpp"
#include "external/coin/IpIpoptApplication.hpp"
#include "external/coin/IpSolveStatistics.hpp"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <boost/io/ios_state.hpp>

using namespace Ipopt;
using namespace Optimizer;

Equilibrium::Equilibrium(const Database &DB, const evalconditions &conds, const SmartPtr<IpoptApplication> &solver)
: sourcename(DB.get_info()), conditions(conds) {
	BOOST_LOG_NAMED_SCOPE("Equilibrium::Equilibrium");
	logger opt_log(journal::keywords::channel = "optimizer");
	BOOST_LOG_SEV(opt_log, debug) << "enter ctor";
	Phase_Collection phase_col;
	boost::timer::cpu_timer timer; // tracking wall clock time for the solve
	for (auto i = DB.get_phase_iterator(); i != DB.get_phase_iterator_end(); ++i) {
		if (conditions.phases.find(i->first) != conditions.phases.end()) {
			if (conditions.phases.at(i->first) == PhaseStatus::ENTERED) phase_col[i->first] = i->second;
		}
	}

	const Phase_Collection::const_iterator phase_iter = phase_col.cbegin();
	const Phase_Collection::const_iterator phase_end = phase_col.cend();

	// TODO: check validity of conditions
	if (phase_iter == phase_end) {
		// No phases are entered
		BOOST_THROW_EXCEPTION(equilibrium_error() << str_errinfo("No phases are entered"));
	}

	timer.start();
	// Create NLP
	SmartPtr<TNLP> mynlp = new GibbsOpt(DB, conditions);
	BOOST_LOG_SEV(opt_log, debug) << "return from GibbsOpt ctor";
	ApplicationReturnStatus status;
	status = solver->OptimizeTNLP(mynlp);
	BOOST_LOG_SEV(opt_log, debug) << "return from GibbsOpt::OptimizeTNLP";
	timer.stop();

	if (status == Solve_Succeeded || status == Solved_To_Acceptable_Level) {
		BOOST_LOG_SEV(opt_log, debug) << "Ipopt returned successfully";
		Number final_obj;
		/* The dynamic_cast allows us to use the get_result() function.
		 * It is not exposed by the TNLP base class.
		 */
		GibbsOpt* opt_ptr = dynamic_cast<GibbsOpt*> (Ipopt::GetRawPtr(mynlp));
		if (!opt_ptr)
		{
			BOOST_LOG_SEV(opt_log, critical) << "Internal memory error from dynamic_cast<GibbsOpt*>";
			BOOST_THROW_EXCEPTION(equilibrium_error() << str_errinfo("Internal memory error") << specific_errinfo("dynamic_cast<GibbsOpt*>"));
		}
		BOOST_LOG_SEV(opt_log, debug) << "Attempting get_result()";
		result = opt_ptr->get_result();

		if (IsValid(solver->Statistics())) {
			result.itercount = solver->Statistics()->IterationCount();
		}
		else {
			// No statistics for the solve, must have been trivial
			result.itercount = 0;
		}

		result.walltime = timer.elapsed().wall * 1e-9; // timer.elapsed().wall is in nanoseconds
		result.N = conditions.statevars.find('N')->second; // TODO: this needs to be done in a more general way for unknown N
	}
	else {
		BOOST_LOG_SEV(opt_log, critical) << "Failed to construct Equilibrium object";
		BOOST_THROW_EXCEPTION(equilibrium_error() << str_errinfo("Solver failed to find equilibrium"));
	}
	BOOST_LOG_SEV(opt_log, debug) << "exit ctor";
}

std::string Equilibrium::print() const {
	BOOST_LOG_NAMED_SCOPE("Equilibrium::print");
	logger opt_log(journal::keywords::channel = "optimizer");
	BOOST_LOG_SEV(opt_log, debug) << "enter function";
	std::stringstream stream;
	boost::io::ios_flags_saver  ifs( stream ); // preserve original state of the stream once we leave scope
	stream << "Output from LIBGIBBS, equilibrium number = ??" << std::endl;
	//std::string walltime = boost::timer::format(result.walltime, 3, "%w");
	stream << "Solved in " << result.itercount << " iterations (" << result.walltime << "secs)" << std::endl;
	stream << "Conditions:" << std::endl;

	// We want the individual phase information to appear AFTER
	// the global system data, but the most efficient ordering
	// requires us to iterate through all phases first.
	// The simple solution is to save the output to a temporary
	// buffer, and then flush it to the output stream later.
	std::stringstream temp_buf;
    temp_buf << std::scientific;

	const auto sv_end = conditions.statevars.cend();
	const auto xf_end = conditions.xfrac.cend();
	for (auto i = conditions.xfrac.cbegin(); i !=xf_end; ++i) {
		stream << "X(" << i->first << ")=" << i->second << ", ";
	}
	for (auto i = conditions.statevars.cbegin(); i != sv_end; ++i) {
		stream << i->first << "=" << i->second;
		// if this isn't the last one, add a comma
		if (std::distance(i,sv_end) != 1) stream << ", ";
	}
	stream << std::endl;
	// should be c + 2 - conditions, where c is the number of components
	size_t dof = conditions.elements.size() + 2 - (conditions.xfrac.size()+1) - conditions.statevars.size();
	stream << "DEGREES OF FREEDOM " << dof << std::endl;

	stream << std::endl;

	double T,P,N,energy;
	T = conditions.statevars.find('T')->second;
	P = conditions.statevars.find('P')->second;
	N = conditions.statevars.find('N')->second;
	energy = result.energy();
	stream << "Temperature " << T << " K (" << (T-273.15) << " C), " << "Pressure " << P << " Pa" << std::endl;

	stream << std::scientific; // switch to scientific notation for doubles
    stream << "Number of moles of components " << N << ", Mass ????" << std::endl;
    stream << "Total Gibbs energy " << energy << " Enthalpy ???? " << "Volume ????" << std::endl;

    stream << std::endl;

    // double/double pair is for separate storage of numerator/denominator pieces of fraction
    std::map<std::string,std::pair<double,double>> global_comp;
    const auto cond_spec_begin = result.conditions.elements.cbegin();
    const auto cond_spec_end = result.conditions.elements.cend();
    for (auto i = result.phases.begin(); i != result.phases.end(); ++i) {
    	const auto subl_begin = i->second.sublattices.cbegin();
    	const auto subl_end = i->second.sublattices.cend();
    	const double phasefrac = i->second.f;

    	std::map<std::string,std::pair<double,double>> phase_comp;
    	temp_buf << i->first << "\tStatus " << enumToString(i->second.status) << "  Driving force ????" << std::endl; // phase name
    	temp_buf << "Number of moles " << phasefrac * N << ", Mass ???? ";
    	temp_buf << "Mole fractions:" << std::endl;

    	for (auto j = cond_spec_begin; j != cond_spec_end; ++j) {
    		if (*j == "VA") continue; // don't include vacancies in chemical potential
    		double mu = i->second.chemical_potential(*j, result.variables, result.conditions);
    		temp_buf << "MU(" << *j << ") = " << mu << std::endl;
    	}

    	for (auto j = subl_begin; j != subl_end; ++j) {
    		const double stoi_coef = j->sitecount;
    		const double den = stoi_coef;
    		const auto spec_begin = j->components.cbegin();
    		const auto spec_end = j->components.cend();
    		const auto vacancy_iterator = (j->components).find("VA"); // this may equal spec_end
    		/*
    		 * To make sure all mole fractions sum to 1,
    		 * we have to normalize everything using the same
    		 * sublattices. That means even species which don't
    		 * appear in a given sublattice need to have that
    		 * sublattice's coefficient appear in its denominator.
    		 * To accomplish this task, we perform two loops in each sublattice:
    		 * 1) all species (except VA), to add to the denominator
    		 * 2) only species in that sublattice, to add to the numerator
    		 * With this method, all mole fractions will sum properly.
    		 */
    		for (auto k = cond_spec_begin; k != cond_spec_end; ++k) {
    			if (*k == "VA") continue; // vacancies don't contribute to mole fractions
				if (vacancy_iterator != spec_end) {
					phase_comp[*k].second += den * (1 - vacancy_iterator->second.site_fraction);
				}
				else {
					phase_comp[*k].second += den;
				}
    		}
    		for (auto k = spec_begin; k != spec_end; ++k) {
    			if (k->first == "VA") continue; // vacancies don't contribute to mole fractions
                double num = k->second.site_fraction * stoi_coef;
                phase_comp[k->first].first += num;
    		}
    	}
    	/* We've summed up over all sublattices in this phase.
    	 * Now add this phase's contribution to the overall composition.
    	 */
    	for (auto j = cond_spec_begin; j != cond_spec_end; ++j) {
    		if (*j == "VA") continue; // vacancies don't contribute to mole fractions
    		global_comp[*j].first += phasefrac * (phase_comp[*j].first / phase_comp[*j].second);
    		global_comp[*j].second += phasefrac;
    	}

    	const auto cmp_begin = phase_comp.cbegin();
    	const auto cmp_end = phase_comp.cend();
    	for (auto g = cmp_begin; g != cmp_end; ++g) {
    		temp_buf << g->first << " " << (g->second.first / g->second.second) << "  ";
    	}
    	temp_buf << std::endl << "Constitution:" << std::endl;

    	for (auto j = subl_begin; j != subl_end; ++j) {
    		double stoi_coef = j->sitecount;
    		temp_buf << "Sublattice " << std::distance(subl_begin,j)+1 << ", Number of sites " << stoi_coef << std::endl;
    		const auto spec_begin = j->components.cbegin();
    		const auto spec_end = j->components.cend();
    		for (auto k = spec_begin; k != spec_end; ++k) {
                temp_buf << k->first << " " << k->second.site_fraction << " ";
    		}
    		temp_buf << std::endl;
    	}

    	// if we're at the last phase, don't add an extra newline
    	if (std::distance(i,result.phases.end()) != 1) temp_buf << std::endl;
    }
    const auto glob_begin = global_comp.cbegin();
    const auto glob_end = global_comp.cend();
    stream << "Component\tMoles\tM-Fraction\tActivity\tPotential\tRef.state" << std::endl;
    for (auto h = glob_begin; h != glob_end; ++h) {
    	double molefrac = h->second.first / h->second.second;
    	stream << h->first << "\t" <<  molefrac * N << "\t" << molefrac << "\t????\t" << "??" << "\tSER" << std::endl;
    }
    stream << std::endl;

    stream << temp_buf.rdbuf(); // include the temporary buffer with all the phase data

    BOOST_LOG_SEV(opt_log, debug) << "returning";
	return (const std::string)stream.str();
}
