#include "DeterministicNoiseSimulator.hpp"

using CN = dd::ComplexNumbers;

dd::Package::mEdge DeterministicNoiseSimulator::makeZeroDensityOperator(dd::QubitCount n) {
    auto f = dd::Package::mEdge::one;
    for (std::size_t p = 0; p < n; p++) {
        f = dd->makeDDNode(static_cast<dd::Qubit>(p), std::array{f, dd::Package::mEdge::zero, dd::Package::mEdge::zero, dd::Package::mEdge::zero});
    }
    return f;
}

std::map<std::string, double> DeterministicNoiseSimulator::DeterministicSimulate() {
    const unsigned short n_qubits = qc->getNqubits();
    std::map<unsigned int, bool> classic_values;

    density_root_edge = makeZeroDensityOperator(n_qubits);
    dd->incRef(density_root_edge);


    for (auto const &op : *qc) {
        if (!op->isUnitary() && !(op->isClassicControlledOperation())) {
            if (auto *nu_op = dynamic_cast<qc::NonUnitaryOperation *>(op.get())) {
                if (nu_op->getName()[0] == 'M') {
                    auto quantum = nu_op->getTargets();
                    auto classic = nu_op->getClassics();

                    if (quantum.size() != classic.size()) {
                        std::cerr << "[ERROR] Measurement: Sizes of quantum and classic register mismatch.\n";
                        std::exit(1);
                    }

                    for (unsigned int i = 0; i < quantum.size(); ++i) {
                        auto result = MeasureOneCollapsing(quantum[i]);
                        assert(result == '0' || result == '1');
                        classic_values[classic[i]] = result == '1';
                    }
                } else if (strcmp(nu_op->getName(), "Rst") == 0) {
                    // Reset qubit
                    printf("Warning: Reset is currently not supported");
                    continue;
                } else {
                    //Skipping barrier
                    if ((strcmp(nu_op->getName(), "Barr") == 0)) {
                        continue;
                    }
                    std::cerr << "[ERROR] Unsupported non-unitary functionality: \"" << nu_op->getName() << "\""
                              << std::endl;
                    std::exit(1);
                }
            } else {
                std::cerr << "[ERROR] Dynamic cast to NonUnitaryOperation failed." << std::endl;
                std::exit(1);
            }
            dd->garbageCollect();
        } else {
            dd::Package::mEdge dd_op = {};
            qc::Targets targets;
            dd::Controls controls;
            if (op->isClassicControlledOperation()) {
                // Check if the operation is controlled by a classical register
                auto *classic_op = dynamic_cast<qc::ClassicControlledOperation *>(op.get());
                bool conditionTrue = true;
                auto expValue = classic_op->getExpectedValue();

                for (unsigned int i = classic_op->getControlRegister().first;
                     i < classic_op->getControlRegister().second; i++) {
                    if (classic_values[i] != (expValue % 2)) {
                        conditionTrue = false;
                        break;
                    } else {
                        expValue = expValue >> 1u;
                    }
                }
                dd_op = classic_op->getOperation()->getDD(dd);
                targets = classic_op->getOperation()->getTargets();
                assert(targets.size() == 1);
                controls = classic_op->getOperation()->getControls();
                if (!conditionTrue) {
                    continue;
                }
            } else {
                dd_op = op->getDD(dd);
                targets = op->getTargets();
                assert(targets.size() == 1);
                controls = op->getControls();
            }
            // Applying the operation to the density matrix
            dd::Package::mEdge tmp0 = dd->multiply(dd->multiply(dd_op, density_root_edge), dd->conjugateTranspose(dd_op));
            dd->incRef(tmp0);
            dd->decRef(density_root_edge);
            density_root_edge = tmp0;

            if (weird_value_i_dont_understand_but_something_with_sequential) { // was `stochastic_runs == -2`
                [[maybe_unused]] auto cache_size_before = dd->cn.cacheCount();

                auto seq_targets = targets;
                for( auto const& control : controls) {
                    targets.push_back(control.qubit);
                }
                apply_det_noise_sequential(seq_targets);

                [[maybe_unused]] auto cache_size_after = dd->cn.cacheCount();
                assert(cache_size_after == cache_size_before);
            } else {
                auto maxDepth = op->getTargets().front();
                auto control_qubits = op->getControls();
                for (auto &control : control_qubits) {
                    if (control.qubit < maxDepth) {
                        maxDepth = control.qubit;
                    }
                }
                [[maybe_unused]] auto cache_size_before = dd->cn.cacheCount();

                auto tmp2 = ApplyNoiseEffects(density_root_edge, op, 0);
                if (!tmp2.w.approximatelyZero()) {
                    dd::Complex c = dd->cn.lookup(tmp2.w);
                    dd->cn.returnToCache(tmp2.w);
                    tmp2.w = c;
                }

                [[maybe_unused]] auto cache_size_after = dd->cn.cacheCount();
                assert(cache_size_after == cache_size_before);

                dd->incRef(tmp2);
                dd->decRef(density_root_edge);
                density_root_edge = tmp2;
            }
        }
    }
    dd->garbageCollect();
    return AnalyseState(n_qubits);
}

dd::Package::mEdge DeterministicNoiseSimulator::ApplyNoiseEffects(dd::Package::mEdge density_op, const std::unique_ptr<qc::Operation>& op, unsigned char maxDepth) {
    if (density_op.p->v < maxDepth || density_op.isTerminal()) {
        dd::Package::mEdge tmp{};
        if (!density_op.w.approximatelyZero()) {
            tmp.w = dd->cn.getCached(dd::CTEntry::val(density_op.w.r), dd::CTEntry::val(density_op.w.i));
        } else {
            tmp.w = dd::Complex::zero;
        }
        if (density_op.isTerminal()) {
            return dd::Package::mEdge::terminal(tmp.w);
        } else {
            tmp.p = density_op.p;
            return tmp;
        }
    }

    // TODO change this to use controls and targets directly
    std::array<short, 100> line{};
    line.fill(-1);
    for (auto const& target: op->getTargets()) {
        line.at(target) = 2;
    }
    for (auto const& control: op->getControls()) {
        line.at(control.qubit) = control.type == dd::Control::Type::pos ? 1 : 0;
    }

    std::array<dd::Package::mEdge, 4> new_edges{};
    for (int i = 0; i < 4; i++) {
        if (density_op.p->e[i].w.approximatelyZero()) {
            // applyNoiseEffects2 returns a pointer to the terminal node with edge weight 0
            new_edges[i] = dd::Package::mEdge::terminal(density_op.p->e[i].w);
            continue;
        }

        // Check if the target of the current edge is in the Compute table. Note that I check for the target node of
        // the current edge if noise needs to be applied or not
        dd::Package::mEdge tmp = noiseLookup(density_op.p->e[i], line.data(), getNumberOfQubits());
        //tmp = dd->noiseOperationTable.lookup(getNumberOfQubits(), )

        if (tmp.p != nullptr) {
            new_edges[i] = tmp;
            continue;
        }

        new_edges[i] = ApplyNoiseEffects(density_op.p->e[i], op, maxDepth);

        // Adding the operation to the operation table
        noiseInsert(density_op.p->e[i], line.data(), new_edges[i], getNumberOfQubits());
    }

    //if (line[density_op.p->v] > 0) {
    if (op->actsOn(density_op.p->v)) {
        //todo when A and D noise is active and prob is 0, I lose cached variables
        for (auto const& type : gate_noise_types) {
            switch (type) {
                case 'A':
                    ApplyAmplitudeDampingToNode(new_edges);
                    break;
                case 'P':
                    ApplyPhaseFlipToNode(new_edges);
                    break;
                //case 'B':
                //    applyBitFlipToNode(new_edges);
                //    break;
                case 'D':
                    ApplyDepolaritationToNode(new_edges);
                    break;
                default:
                    throw std::runtime_error(std::string("Unknown gate noise type '") + type + "'");
            }
        }
    }

    qc::MatrixDD tmp = dd->makeDDNode(density_op.p->v, new_edges, true);

    // Multiplying the old edge weight with the new one
    if (!tmp.w.approximatelyZero()) {
        CN::mul(tmp.w, tmp.w, density_op.w);
    }
    return tmp;
}

void DeterministicNoiseSimulator::ApplyPhaseFlipToNode(std::array<dd::Package::mEdge, 4>& e) {
    double probability = noise_probability;
    dd::Complex complex_prob = dd->cn.getCached();

    //e[0] = e[0]

    //e[1] = (1-2p)*e[1]
    if (!e[1].w.approximatelyZero()) {
        complex_prob.r->value = 1 - 2 * probability;
        complex_prob.i->value = 0;
        CN::mul(e[1].w, complex_prob, e[1].w);
    }

    //e[2] = (1-2p)*e[2]
    if (!e[2].w.approximatelyZero()) {
        if (e[1].w.approximatelyZero()) {
            complex_prob.r->value = 1 - 2 * probability;
            complex_prob.i->value = 0;
        }
        CN::mul(e[2].w, complex_prob, e[2].w);
    }

    //e[3] = e[3]

    dd->cn.returnToCache(complex_prob);
}

void DeterministicNoiseSimulator::ApplyAmplitudeDampingToNode(std::array<dd::Package::mEdge, 4>& e) {
    double probability = noise_probability * 2;
    dd::Complex complex_prob = dd->cn.getCached();
    dd::Package::mEdge helper_edge[1];
    helper_edge[0].w = dd->cn.getCached();

    // e[0] = e[0] + p*e[3]
    if (!e[3].w.approximatelyZero()) {
        complex_prob.r->value = probability;
        complex_prob.i->value = 0;
        if (!e[0].w.approximatelyZero()) {
            CN::mul(helper_edge[0].w, complex_prob, e[3].w);
            helper_edge[0].p = e[3].p;
            dd::Edge tmp = dd->add(e[0], helper_edge[0]); // was dd->add2(...)
            if (!e[0].w.approximatelyZero()) {
                dd->cn.returnToCache(e[0].w);
            }
            e[0] = tmp;
        } else {
            e[0].w = dd->cn.getCached();
            CN::mul(e[0].w, complex_prob, e[3].w);
            e[0].p = e[3].p;
        }
    }

    //e[1] = sqrt(1-p)*e[1]
    if (!e[1].w.approximatelyZero()) {
        complex_prob.r->value = sqrt(1 - probability);
        complex_prob.i->value = 0;
        CN::mul(e[1].w, complex_prob, e[1].w);
    }

    //e[2] = sqrt(1-p)*e[2]
    if (!e[2].w.approximatelyZero()) {
        if (e[1].w.approximatelyZero()) {
            complex_prob.r->value = sqrt(1 - probability);
            complex_prob.i->value = 0;
        }
        CN::mul(e[2].w, complex_prob, e[2].w);
    }


    //e[3] = (1-p)*e[3]
    if (!e[3].w.approximatelyZero()) {
        complex_prob.r->value = 1 - probability;
        CN::mul(e[3].w, complex_prob, e[3].w);
    }

    dd->cn.returnToCache(helper_edge[0].w);
    dd->cn.returnToCache(complex_prob);
}


void DeterministicNoiseSimulator::ApplyDepolaritationToNode(std::array<dd::Package::mEdge, 4>& e) {
    double probability = noise_probability;
    dd::Package::mEdge helper_edge[2];
    helper_edge[0].w = dd->cn.getCached();
    helper_edge[1].w = dd->cn.getCached();
    dd::Complex complex_prob = dd->cn.getCached();

    //todo I don't have to save all edges
    dd::Package::mEdge old_edges[4];
    for (int i = 0; i < 4; i++) {
        if (!e[i].w.approximatelyZero()) {
            old_edges[i].w = dd->cn.getCached(dd::CTEntry::val(e[i].w.r), dd::CTEntry::val(e[i].w.i));
            old_edges[i].p = e[i].p;
        } else {
            old_edges[i] = e[i];
        }
    }

    //e[0] = 0.5*((2-p)*e[0] + p*e[3])
    complex_prob.i->value = 0;
    // first check if e[0] or e[1] != 0
    if (!old_edges[0].w.approximatelyZero() || !old_edges[3].w.approximatelyZero()) {
        if (!old_edges[0].w.approximatelyZero() && old_edges[3].w.approximatelyZero()) {
            complex_prob.r->value = (2 - probability) * 0.5;
            CN::mul(e[0].w, complex_prob, old_edges[0].w);
            e[0].p = old_edges[0].p;
        } else if (old_edges[0].w.approximatelyZero() && !old_edges[3].w.approximatelyZero()) {
            e[0].w = dd->cn.getCached();
            complex_prob.r->value = probability * 0.5;
            CN::mul(e[0].w, complex_prob, old_edges[3].w);
            e[0].p = old_edges[3].p;
        } else {
            complex_prob.r->value = (2 - probability) * 0.5;
            CN::mul(helper_edge[0].w, complex_prob, old_edges[0].w);
            complex_prob.r->value = probability * 0.5;
            CN::mul(helper_edge[1].w, complex_prob, old_edges[3].w);
            helper_edge[0].p = old_edges[0].p;
            helper_edge[1].p = old_edges[3].p;
            dd->cn.returnToCache(e[0].w);
            e[0] = dd->add(helper_edge[0], helper_edge[1]); // was dd->add2(...)
        }
    }
    //e[1]=1-p*e[1]
    if (!e[1].w.approximatelyZero()) {
        complex_prob.r->value = 1 - probability;
        CN::mul(e[1].w, e[1].w, complex_prob);
    }
    //e[2]=1-p*e[2]
    if (!e[2].w.approximatelyZero()) {
        if (e[1].w.approximatelyZero()) {
            complex_prob.r->value = 1 - probability;
        }
        CN::mul(e[2].w, e[2].w, complex_prob);
    }

    //e[3] = 0.5*((2-p)*e[3] + p*e[0])
    if (!old_edges[0].w.approximatelyZero() || !old_edges[3].w.approximatelyZero()) {
        if (!old_edges[0].w.approximatelyZero() && old_edges[3].w.approximatelyZero()) {
            e[3].w = dd->cn.getCached();
            complex_prob.r->value = probability * 0.5;
            CN::mul(e[3].w, complex_prob, old_edges[0].w);
            e[3].p = old_edges[0].p;
        } else if (old_edges[0].w.approximatelyZero() && !old_edges[3].w.approximatelyZero()) {
            complex_prob.r->value = (2 - probability) * 0.5;
            CN::mul(e[3].w, complex_prob, old_edges[3].w);
            e[3].p = old_edges[3].p;
        } else {
            complex_prob.r->value = probability * 0.5;
            CN::mul(helper_edge[0].w, complex_prob, old_edges[0].w);
            complex_prob.r->value = (2 - probability) * 0.5;
            CN::mul(helper_edge[1].w, complex_prob, old_edges[3].w);
            helper_edge[0].p = old_edges[0].p;
            helper_edge[1].p = old_edges[3].p;
            dd->cn.returnToCache(e[3].w);
            e[3] = dd->add(helper_edge[0], helper_edge[1]); // was dd->add2(...)
        }
    }
    for (auto &old_edge : old_edges) {
        if (!old_edge.w.approximatelyZero()) {
            dd->cn.returnToCache(old_edge.w);
        }
    }
//    helper_edge[0].w.r->next->next->next->next->next->next = ComplexCache_Avail;
    dd->cn.returnToCache(helper_edge[0].w);
    dd->cn.returnToCache(helper_edge[1].w);
    dd->cn.returnToCache(complex_prob);
}

std::map<std::string, double> DeterministicNoiseSimulator::AnalyseState(int nr_qubits) {
    std::map<std::string, double> measure_result = {};
    double p0, p1, imaginary;
    double long global_probability;

    dd::Edge original_state = root_edge;
    for (int m = 0; m < pow(2, nr_qubits); m++) {
        int current_result = m;
        global_probability = dd::CTEntry::val(root_edge.w.r);
        std::string result_string = intToString(m, '1');
        dd::Edge cur = root_edge;
        for (int i = 0; i < nr_qubits; ++i) {
            if (cur.p->v != -1) {
                imaginary = dd::CTEntry::val(cur.p->e[0].w.i) + dd::CTEntry::val(cur.p->e[3].w.i);
                p0 = dd::CTEntry::val(cur.p->e[0].w.r);
                p1 = dd::CTEntry::val(cur.p->e[3].w.r);
            } else {
                global_probability = 0;
                break;
            }

            if (current_result % 2 == 0) {
                cur = cur.p->e[0];
                global_probability *= p0;
            } else {
                cur = cur.p->e[3];
                global_probability *= p1;
            }
            current_result = current_result >> 1;
        }
        if (global_probability > 0.01) {
            std::cout << "Measured state=|" << result_string << "> probability=" << global_probability << "\n"
                      << std::flush;
        }
        measure_result.insert({result_string, global_probability});
    }
    return measure_result;
}

void DeterministicNoiseSimulator::generate_gate(dd::Package::mEdge *pointer_for_matrices, char noise_type, dd::Qubit target) {
    std::array<dd::GateMatrix, 4> idle_noise_gate{};
    dd::ComplexValue tmp = {};

    double probability = noise_probability;

    switch (noise_type) {
        // bitflip
        //      (sqrt(1-probability)    0           )       (0      sqrt(probability))
        //  e0= (0            sqrt(1-probability)   ), e1=  (sqrt(probability)      0)
        case 'B': {
            tmp.r = std::sqrt(1 - probability) * dd::complex_one.r;
            idle_noise_gate[0][0] = idle_noise_gate[0][3] = tmp;
            idle_noise_gate[0][1] = idle_noise_gate[0][2] = dd::complex_zero;
            tmp.r = std::sqrt(probability) * dd::complex_one.r;
            idle_noise_gate[1][1] = idle_noise_gate[1][2] = tmp;
            idle_noise_gate[1][0] = idle_noise_gate[1][3] = dd::complex_zero;

            pointer_for_matrices[0] = dd->makeGateDD(idle_noise_gate[0], getNumberOfQubits(), target);
            pointer_for_matrices[1] = dd->makeGateDD(idle_noise_gate[1], getNumberOfQubits(), target);
            break;

        }
            // phase flip
            //      (sqrt(1-probability)    0           )       (sqrt(probability)      0)
            //  e0= (0            sqrt(1-probability)   ), e1=  (0      -sqrt(probability))
        case 'P': {
            tmp.r = std::sqrt(1 - probability) * dd::complex_one.r;
            idle_noise_gate[0][0] = idle_noise_gate[0][3] = tmp;
            idle_noise_gate[0][1] = idle_noise_gate[0][2] = dd::complex_zero;
            tmp.r = std::sqrt(probability) * dd::complex_one.r;
            idle_noise_gate[1][0] = tmp;
            tmp.r *= -1;
            idle_noise_gate[1][3] = tmp;
            idle_noise_gate[1][1] = idle_noise_gate[1][2] = dd::complex_zero;

            pointer_for_matrices[0] = dd->makeGateDD(idle_noise_gate[0], getNumberOfQubits(), target);
            pointer_for_matrices[1] = dd->makeGateDD(idle_noise_gate[1], getNumberOfQubits(), target);


            break;
        }
            // amplitude damping
            //      (1      0           )       (0      sqrt(probability))
            //  e0= (0      sqrt(1-probability)   ), e1=  (0      0      )
        case 'A': {
            tmp.r = std::sqrt(1 - probability * 2) * dd::complex_one.r;
            idle_noise_gate[0][0] = dd::complex_one;
            idle_noise_gate[0][1] = idle_noise_gate[0][2] = dd::complex_zero;
            idle_noise_gate[0][3] = tmp;


            tmp.r = std::sqrt(probability * 2) * dd::complex_one.r;
            idle_noise_gate[1][0] = idle_noise_gate[1][3] = idle_noise_gate[1][2] = dd::complex_zero;
            idle_noise_gate[1][1] = tmp;

            pointer_for_matrices[0] = dd->makeGateDD(idle_noise_gate[0], getNumberOfQubits(), target);
            pointer_for_matrices[1] = dd->makeGateDD(idle_noise_gate[1], getNumberOfQubits(), target);
            break;
        }
            // depolarization
        case 'D': {
            tmp.r = std::sqrt(1 - ((3 * probability) / 4)) * dd::complex_one.r;
            //                   (1 0)
            // sqrt(1- ((3p)/4))*(0 1)
            idle_noise_gate[0][0] = idle_noise_gate[0][3] = tmp;
            idle_noise_gate[0][1] = idle_noise_gate[0][2] = dd::complex_zero;

            pointer_for_matrices[0] = dd->makeGateDD(idle_noise_gate[0], getNumberOfQubits(), target);

            //            (0 1)
            // sqrt(probability/4))*(1 0)
            tmp.r = std::sqrt(probability / 4) * dd::complex_one.r;
            idle_noise_gate[1][1] = idle_noise_gate[1][2] = tmp;
            idle_noise_gate[1][0] = idle_noise_gate[1][3] = dd::complex_zero;

            pointer_for_matrices[1] = dd->makeGateDD(idle_noise_gate[1], getNumberOfQubits(), target);

            //            (1 0)
            // sqrt(probability/4))*(0 -1)
            tmp.r = std::sqrt(probability / 4) * dd::complex_one.r;
            idle_noise_gate[2][0] = tmp;
            tmp.r = tmp.r * -1;
            idle_noise_gate[2][3] = tmp;
            idle_noise_gate[2][1] = idle_noise_gate[2][2] = dd::complex_zero;

            pointer_for_matrices[3] = dd->makeGateDD(idle_noise_gate[2], getNumberOfQubits(), target);

            //            (0 -i)
            // sqrt(probability/4))*(i 0)
            tmp.r = dd::complex_zero.r;
            tmp.i = std::sqrt(probability / 4) * 1;
            idle_noise_gate[3][2] = tmp;
            tmp.i = tmp.i * -1;
            idle_noise_gate[3][1] = tmp;
            idle_noise_gate[3][0] = idle_noise_gate[3][3] = dd::complex_zero;

            pointer_for_matrices[2] = dd->makeGateDD(idle_noise_gate[3], getNumberOfQubits(), target);
            break;
        }
        default:
            throw std::runtime_error("Unknown noise effect received.");
    }
}

void DeterministicNoiseSimulator::apply_det_noise_sequential(const qc::Targets& targets) {
    dd::Package::mEdge tmp = {};
    dd::Package::mEdge ancillary_edge_1 = {};
    dd::Package::mEdge idle_operation[4];

    // Iterate over qubits and check if the qubit had been used
    for (auto target_qubit : targets) {
        for (auto const& type : gate_noise_types) {
            generate_gate(idle_operation, type, target_qubit);
            tmp.p = nullptr;
            //Apply all noise matrices of the current noise effect
            for (int m = 0; m < noise_effects.find(type)->second; m++) {
                auto tmp0 = dd->multiply(dd->multiply(idle_operation[m], density_root_edge), dd->conjugateTranspose(idle_operation[m]));
                if (tmp.p == nullptr) {
                    tmp = tmp0;
                } else {
                    tmp = dd->add(tmp0, tmp);
                }
            }
            dd->decRef(density_root_edge);
            dd->incRef(tmp);
            density_root_edge = tmp;
        }
    }
}

std::string DeterministicNoiseSimulator::intToString(long target_number, char value) const {
    if (target_number < 0) {
        assert(target_number == -1);
        return (std::string("F"));
    }
    auto qubits = getNumberOfQubits();
    std::string path(qubits, '0');
    auto number = (unsigned long) target_number;
    for (int i = 1; i <= qubits; i++) {
        if (number % 2) {
            path[qubits - i] = value;
        }
        number = number >> 1u;
    }
    return path;
}

//
// TODO get rid of the line parameter
//

void DeterministicNoiseSimulator::noiseInsert(const dd::Package::mEdge &a, const short *line, const dd::Package::mEdge &r, unsigned short nQubits) {
    dd::ComplexValue aw{dd::CTEntry::val(a.w.r), dd::CTEntry::val(a.w.i)};
    const unsigned long i = noiseHash(a.p, aw, line, nQubits);

    assert(((std::uintptr_t) r.w.r & 1u) == 0 && ((std::uintptr_t) r.w.i & 1u) == 0);

    std::memcpy(NoiseTable[i].line, line, (nQubits) * sizeof(short));

    NoiseTable[i].a = a.p;
    NoiseTable[i].aw = aw;
    NoiseTable[i].r = r.p;
    NoiseTable[i].rw.r = r.w.r->value;
    NoiseTable[i].rw.i = r.w.i->value;
}

dd::Package::mEdge DeterministicNoiseSimulator::noiseLookup(const dd::Package::mEdge &a, const short *line, unsigned short nQubits) {
    // Lookup a computation in the compute table
    // return NULL if not a match else returns result of prior computation
    dd::Package::mEdge r{nullptr, {nullptr, nullptr}};

    dd::ComplexValue aw{dd::CTEntry::val(a.w.r), dd::CTEntry::val(a.w.i)};

    const unsigned long i = noiseHash(a.p, aw, line, nQubits);

    if (NoiseTable[i].a != a.p || NoiseTable[i].aw != aw) return r;
    if (std::memcmp(&NoiseTable[i].line[a.p->v], &line[a.p->v], (nQubits-a.p->v) * sizeof(short)) != 0) {
        return r;
    }

    r.p = NoiseTable[i].r;

    if (std::abs(NoiseTable[i].rw.r) < dd::ComplexTable<>::tolerance() && std::fabs(NoiseTable[i].rw.i) < dd::ComplexTable<>::tolerance()) {
        return dd::Package::mEdge::zero;
    } else {
        r.w = dd->cn.getCached(NoiseTable[i].rw.r, NoiseTable[i].rw.i);
    }
    return r;
}

unsigned long DeterministicNoiseSimulator::noiseHash(dd::Package::mNode* a, const dd::ComplexValue &aw, const short *line, unsigned short nQubits) {
    unsigned long i = 0;
    for (unsigned short j = 0; j <= nQubits; j++) {
//            i = (i << 5u) + (4 * j) + (line[j] * 4);
        i = (i << 3U) + i * j + line[j];
    }
    i += (reinterpret_cast<std::uintptr_t>(a)) + static_cast<unsigned long>(aw.r * 1000000 + aw.i * 2000000);
//        i += (reinterpret_cast<std::uintptr_t>(a)) + static_cast<unsigned long>(aw.r * 100 + aw.i * 200);
//        if ((i & NoiseMASK) == 16279) {
//            printf("\n");
//            volatile int ii = 1;
//        }

    return i & NoiseMASK;
}