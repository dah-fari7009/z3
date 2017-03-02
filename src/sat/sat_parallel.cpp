/*++
Copyright (c) 2017 Microsoft Corporation

Module Name:

    sat_parallel.cpp

    Abstract:

    Utilities for parallel SAT solving.

Author:

    Nikolaj Bjorner (nbjorner) 2017-1-29.

Revision History:

--*/
#include "sat_parallel.h"
#include "sat_clause.h"
#include "sat_solver.h"


namespace sat {

    void parallel::vector_pool::next(unsigned& index) {
        SASSERT(index < m_size);
        unsigned n = index + 2 + get_length(index);
        if (n >= m_size) {
            index = 0;
        }
        else {
            index = n;
        }
    }

    void parallel::vector_pool::reserve(unsigned num_threads, unsigned sz) {
        m_vectors.reset();
        m_vectors.resize(sz, 0);
        m_heads.reset();
        m_heads.resize(num_threads, 0);
        m_at_end.reset();
        m_at_end.resize(num_threads, true);
        m_tail = 0;
        m_size = sz;
    }
    
    void parallel::vector_pool::begin_add_vector(unsigned owner, unsigned n) {
        SASSERT(m_tail < m_size);
        unsigned capacity = n + 2;
        m_vectors.reserve(m_size + capacity, 0);
        IF_VERBOSE(3, verbose_stream() << owner << ": begin-add " << n << " tail: " << m_tail << " size: " << m_size << "\n";);
        for (unsigned i = 0; i < m_heads.size(); ++i) {
            while (m_tail < m_heads[i] && m_heads[i] < m_tail + capacity) {
                next(m_heads[i]);
            }
            m_at_end[i] = false;
        }
        m_vectors[m_tail++] = owner;
        m_vectors[m_tail++] = n;    
    }

    void parallel::vector_pool::add_vector_elem(unsigned e) {
        m_vectors[m_tail++] = e;
    }

    void parallel::vector_pool::end_add_vector() {
        if (m_tail >= m_size) {
            m_tail = 0;
        }
    }


    bool parallel::vector_pool::get_vector(unsigned owner, unsigned& n, unsigned const*& ptr) {
        unsigned head = m_heads[owner];      
        unsigned iterations = 0;
        while (head != m_tail || !m_at_end[owner]) {
            ++iterations;
            SASSERT(head < m_size && m_tail < m_size);            
            bool is_self = owner == get_owner(head);
            next(m_heads[owner]);
            IF_VERBOSE(static_cast<unsigned>(iterations > m_size ? 0 : 3), verbose_stream() << owner << ": [" << head << ":" << m_heads[owner] << "] tail: " << m_tail << "\n";);
            m_at_end[owner] = (m_heads[owner] == m_tail);
            if (!is_self) {
                n = get_length(head);
                ptr = get_ptr(head);
                return true;
            }
            head = m_heads[owner];
        }
        return false;
    }

    parallel::parallel(solver& s): m_scoped_rlimit(s.rlimit()) {}

    parallel::~parallel() {
        for (unsigned i = 0; i < m_solvers.size(); ++i) {            
            dealloc(m_solvers[i]);
        }
    }

    void parallel::init_solvers(solver& s, unsigned num_extra_solvers) {
        unsigned num_threads = num_extra_solvers + 1;
        m_solvers.resize(num_extra_solvers, 0);
        symbol saved_phase = s.m_params.get_sym("phase", symbol("caching"));
        for (unsigned i = 0; i < num_extra_solvers; ++i) {        
            m_limits.push_back(reslimit());
        }
        
        for (unsigned i = 0; i < num_extra_solvers; ++i) {
            s.m_params.set_uint("random_seed", s.m_rand());
            if (i == 1 + num_threads/2) {
                s.m_params.set_sym("phase", symbol("random"));
            }                        
            m_solvers[i] = alloc(sat::solver, s.m_params, m_limits[i]);
            m_solvers[i]->copy(s);
            m_solvers[i]->set_par(this, i);
            m_scoped_rlimit.push_child(&m_solvers[i]->rlimit());            
        }
        s.set_par(this, num_extra_solvers);
        s.m_params.set_sym("phase", saved_phase);        
    }


    void parallel::exchange(solver& s, literal_vector const& in, unsigned& limit, literal_vector& out) {
        if (s.m_par_syncing_clauses) return;
        flet<bool> _disable_sync_clause(s.m_par_syncing_clauses, true);
        #pragma omp critical (par_solver)
        {
            if (limit < m_units.size()) {
                // this might repeat some literals.
                out.append(m_units.size() - limit, m_units.c_ptr() + limit);
            }
            for (unsigned i = 0; i < in.size(); ++i) {
                literal lit = in[i];
                if (!m_unit_set.contains(lit.index())) {
                    m_unit_set.insert(lit.index());
                    m_units.push_back(lit);
                }
            }
            limit = m_units.size();

            _get_phase(s);
            _set_phase(s);
        }
    }

    void parallel::share_clause(solver& s, literal l1, literal l2) {        
        if (s.m_par_syncing_clauses) return;
        flet<bool> _disable_sync_clause(s.m_par_syncing_clauses, true);
        #pragma omp critical (par_solver)
        {
            IF_VERBOSE(3, verbose_stream() << s.m_par_id << ": share " <<  l1 << " " << l2 << "\n";);
            m_pool.begin_add_vector(s.m_par_id, 2);
            m_pool.add_vector_elem(l1.index());
            m_pool.add_vector_elem(l2.index());            
            m_pool.end_add_vector();
        }        
    }

    void parallel::share_clause(solver& s, clause const& c) {        
        if (!enable_add(c) || s.m_par_syncing_clauses) return;
        flet<bool> _disable_sync_clause(s.m_par_syncing_clauses, true);
        unsigned n = c.size();
        unsigned owner = s.m_par_id;
        #pragma omp critical (par_solver)
        {
            IF_VERBOSE(3, verbose_stream() << owner << ": share " <<  c << "\n";);
            m_pool.begin_add_vector(owner, n);                
            for (unsigned i = 0; i < n; ++i) {
                m_pool.add_vector_elem(c[i].index());
            }
            m_pool.end_add_vector();
        }
    }

    void parallel::get_clauses(solver& s) {
        if (s.m_par_syncing_clauses) return;
        flet<bool> _disable_sync_clause(s.m_par_syncing_clauses, true);
        #pragma omp critical (par_solver)
        {
            _get_clauses(s);
        }
    }

    void parallel::_get_clauses(solver& s) {
        unsigned n;
        unsigned const* ptr;
        unsigned owner = s.m_par_id;
        while (m_pool.get_vector(owner, n, ptr)) {
            m_lits.reset();
            bool usable_clause = true;
            for (unsigned i = 0; usable_clause && i < n; ++i) {
                literal lit(to_literal(ptr[i]));                
                m_lits.push_back(lit);
                usable_clause = lit.var() <= s.m_par_num_vars && !s.was_eliminated(lit.var());
            }
            IF_VERBOSE(3, verbose_stream() << s.m_par_id << ": retrieve " << m_lits << "\n";);
            SASSERT(n >= 2);
            if (usable_clause) {
                s.mk_clause_core(m_lits.size(), m_lits.c_ptr(), true);
            }
        }        
    }

    bool parallel::enable_add(clause const& c) const {
        // plingeling, glucose heuristic:
        return (c.size() <= 40 && c.glue() <= 8) || c.glue() <= 2;
    }

    void parallel::_set_phase(solver& s) {
        if (!m_phase.empty()) {
            m_phase.reserve(s.num_vars(), 0);
            for (unsigned i = 0; i < s.num_vars(); ++i) {
                if (s.value(i) != l_undef) {
                    m_phase[i] += (s.value(i) == l_true) ? 1 : -1;
                    continue;
                }
                switch (s.m_phase[i]) {
                case POS_PHASE:
                    m_phase[i]++;
                    break;
                case NEG_PHASE:
                    m_phase[i]--;
                    break;
                default:
                    break;
                }
            }
        }
    }

    void parallel::set_phase(solver& s) {
        #pragma omp critical (par_solver)
        {
            _set_phase(s);
        }
    }

    void parallel::get_phase(solver& s) {
        #pragma omp critical (par_solver)
        {
            _get_phase(s);
        }
    }

    void parallel::_get_phase(solver& s) {
        if (!m_phase.empty()) {
            m_phase.reserve(s.num_vars(), 0);
            for (unsigned i = 0; i < s.num_vars(); ++i) {
                if (m_phase[i] < 0) {
                    s.m_phase[i] = NEG_PHASE;
                }
                else if (m_phase[i] > 0) {
                    s.m_phase[i] = POS_PHASE;
                }
            }
        }
    }

    void parallel::get_phase(local_search& s) {
        #pragma omp critical (par_solver)
        {
            for (unsigned i = 0; i < m_phase.size(); ++i) {
                if (m_phase[i] < 0) {
                    s.set_phase(i, false);
                }
                else if (m_phase[i] > 0) {
                    s.set_phase(i, true);
                }
            }
            m_phase.reserve(s.num_vars(), 0);
        }
    }

    void parallel::set_phase(local_search& s) {
        #pragma omp critical (par_solver)
        {
            m_phase.reserve(s.num_vars(), 0);
            for (unsigned i = 0; i < s.num_vars(); ++i) {
                m_phase[i] += (s.get_phase(i) ? 1 : -1);
            }
        }
    }

    
};

