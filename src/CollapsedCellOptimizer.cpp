#include "CollapsedCellOptimizer.hpp"
#include <assert.h>
#include <random>

CollapsedCellOptimizer::CollapsedCellOptimizer() {}

/*
 * Use the "relax" VBEM algorithm over gene equivalence
 * classes to estimate the latent variables (alphaOut)
 * given the current estimates (alphaIn).
 */
void CellVBEMUpdate_(std::vector<SalmonEqClass>& eqVec,
                     const CollapsedCellOptimizer::SerialVecType& alphaIn,
                     const CollapsedCellOptimizer::SerialVecType& priorAlphas,
                     CollapsedCellOptimizer::SerialVecType& alphaOut) {
  assert(alphaIn.size() == alphaOut.size());

  size_t M = alphaIn.size();
  std::vector<double> expTheta(M);

  double alphaSum = {0.0};
  for (size_t i = 0; i < M; ++i) {
    alphaSum += alphaIn[i] + priorAlphas[i];
  }

  double logNorm;
  if (alphaSum > ::digammaMin) {
    logNorm = boost::math::digamma(alphaSum);
  } else {
    logNorm = 0.0;
  }

  for (size_t i = 0; i < M; ++i) {
    auto ap = alphaIn[i] + priorAlphas[i];
    if (ap > ::digammaMin) {
      expTheta[i] =
        std::exp(boost::math::digamma(ap) - logNorm);
    } else {
      expTheta[i] = 0.0;
    }
    alphaOut[i] = 0.0;
  }

  for (size_t eqID = 0; eqID < eqVec.size(); ++eqID) {
    auto& kv = eqVec[eqID];

    uint32_t count = kv.count;
    const std::vector<uint32_t>& genes = kv.labels;
    size_t groupSize = genes.size();

    // get conditional probabilities
    //const auto& auxs = txpGroupCombinedWeights[eqID];
    std::vector<double> auxs (genes.size(), 1.0);

    if (BOOST_LIKELY(groupSize > 1)) {
      double denom = 0.0;
      for (size_t i = 0; i < groupSize; ++i) {
        auto gid = genes[i];
        auto aux = auxs[i];
        if (expTheta[gid] > 0.0) {
          double v = expTheta[gid] * aux;
          denom += v;
        }
      }

      if (denom > 0.0) {
        double invDenom = count / denom;
        for (size_t i = 0; i < groupSize; ++i) {
          auto gid = genes[i];
          auto aux = auxs[i];
          if (expTheta[gid] > 0.0) {
            double v = expTheta[gid] * aux;
            salmon::utils::incLoop(alphaOut[gid], v * invDenom);
          }
        }// end-for groupsize
      }// end-if denom>0
    } else if (groupSize == 1){
      alphaOut[genes.front()] += count;
    } else{
      std::cerr<<"0 Group size for salmonEqclasses in EM\n"
               <<"Please report this on github";
      exit(1);
    }
  }//end-outer for
}

/*
 * Use the "relax" EM algorithm over gene equivalence
 * classes to estimate the latent variables (alphaOut)
 * given the current estimates (alphaIn).
 */
void CellEMUpdate_(std::vector<SalmonEqClass>& eqVec,
                   const CollapsedCellOptimizer::SerialVecType& alphaIn,
                   CollapsedCellOptimizer::SerialVecType& alphaOut) {
  assert(alphaIn.size() == alphaOut.size());

  for (size_t eqID=0; eqID < eqVec.size(); eqID++) {
    auto& kv = eqVec[eqID];

    uint32_t count = kv.count;
    // for each label in this class
    const std::vector<uint32_t>& genes = kv.labels;
    size_t groupSize = genes.size();

    if (BOOST_LIKELY(groupSize > 1)) {
      double denom = 0.0;
      for (size_t i = 0; i < groupSize; ++i) {
        auto gid = genes[i];
        denom += alphaIn[gid];
      }

      if (denom > 0.0) {
        double invDenom = count / denom;
        for (size_t i = 0; i < groupSize; ++i) {
          auto gid = genes[i];
          double v = alphaIn[gid];
          if (!std::isnan(v)) {
            alphaOut[gid] += v * invDenom;
          }
        }//end-for
      }//end-if denom>0
    }//end-if boost gsize>1
    else if (groupSize == 1){
      alphaOut[genes.front()] += count;
    } else{
      std::cerr<<"0 Group size for salmonEqclasses in EM\n"
               <<"Please report this on github";
      exit(1);
    }
  }//end-outer for
}


double truncateAlphas(VecT& alphas, double cutoff) {
  // Truncate tiny expression values
  double alphaSum = 0.0;

  for (size_t i = 0; i < alphas.size(); ++i) {
    if (alphas[i] < cutoff) {
      alphas[i] = 0.0;
    }
    alphaSum += alphas[i];
  }
  return alphaSum;
}

bool runPerCellEM(double& totalNumFrags, size_t numGenes,
                  CollapsedCellOptimizer::SerialVecType& alphas,
                  const CollapsedCellOptimizer::SerialVecType& priorAlphas,
                  std::vector<SalmonEqClass>& salmonEqclasses,
                  std::shared_ptr<spdlog::logger>& jointlog,
                  bool initUniform, bool useVBEM){

  // An EM termination criterion, adopted from Bray et al. 2016
  uint32_t minIter {50};
  double relDiffTolerance {0.01};
  uint32_t maxIter {10000};
  size_t numClasses = salmonEqclasses.size();

  if ( initUniform ) {
    double uniformPrior = 1.0/numGenes;
    std::fill(alphas.begin(), alphas.end(), uniformPrior);
  }
  CollapsedCellOptimizer::SerialVecType alphasPrime(numGenes, 0.0);

  assert( numGenes == alphas.size() );
  for (size_t i = 0; i < numGenes; ++i) {
    alphas[i] += 0.5;
    alphas[i] *= 1e-3;
  }

  bool converged{false};
  double maxRelDiff = -std::numeric_limits<double>::max();
  size_t itNum = 0;

  // EM termination criteria, adopted from Bray et al. 2016
  double minAlpha = 1e-8;
  double alphaCheckCutoff = 1e-2;
  constexpr double minWeight = std::numeric_limits<double>::denorm_min();

  while (itNum < minIter or (itNum < maxIter and !converged)) {
    if (useVBEM) {
      CellVBEMUpdate_(salmonEqclasses, alphas, priorAlphas, alphasPrime);
    } else {
      CellEMUpdate_(salmonEqclasses, alphas, alphasPrime);
    }

    converged = true;
    maxRelDiff = -std::numeric_limits<double>::max();
    for (size_t i = 0; i < numGenes; ++i) {
      if (alphasPrime[i] > alphaCheckCutoff) {
        double relDiff =
          std::abs(alphas[i] - alphasPrime[i]) / alphasPrime[i];
        maxRelDiff = (relDiff > maxRelDiff) ? relDiff : maxRelDiff;
        if (relDiff > relDiffTolerance) {
          converged = false;
        }
      }
      alphas[i] = alphasPrime[i];
      alphasPrime[i] = 0.0;
    }

    ++itNum;
  }

  // Truncate tiny expression values
  totalNumFrags = truncateAlphas(alphas, minAlpha);

  if (totalNumFrags < minWeight) {
    jointlog->error("Total alpha weight was too small! "
                    "Make sure you ran salmon correctly.");
    jointlog->flush();
    return false;
  }

  return true;
}

bool runBootstraps(size_t numGenes,
                   CollapsedCellOptimizer::SerialVecType& geneAlphas,
                   std::vector<SalmonEqClass>& salmonEqclasses,
                   std::shared_ptr<spdlog::logger>& jointlog,
                   uint32_t numBootstraps,
                   CollapsedCellOptimizer::SerialVecType& variance,
                   bool useAllBootstraps,
                   std::vector<std::vector<double>>& sampleEstimates,
                   bool initUniform){

  // An EM termination criterion, adopted from Bray et al. 2016
  uint32_t minIter {50};
  double relDiffTolerance {0.01};
  uint32_t maxIter {10000};
  size_t numClasses = salmonEqclasses.size();

  CollapsedCellOptimizer::SerialVecType mean(numGenes, 0.0);
  CollapsedCellOptimizer::SerialVecType squareMean(numGenes, 0.0);
  CollapsedCellOptimizer::SerialVecType alphas(numGenes, 0.0);
  CollapsedCellOptimizer::SerialVecType alphasPrime(numGenes, 0.0);

  //extracting weight of eqclasses for making discrete distribution
  uint32_t totalNumFrags = 0;
  std::vector<uint64_t> eqCounts;
  for (auto& eqclass: salmonEqclasses) {
    totalNumFrags += eqclass.count;
    eqCounts.emplace_back(eqclass.count);
  }

  // Multinomial Sampler
  std::random_device rd;
  std::mt19937 gen(rd());
  std::discrete_distribution<uint64_t> csamp(eqCounts.begin(),
                                             eqCounts.end());

  uint32_t bsNum {0};
  while ( bsNum++ < numBootstraps) {
    csamp.reset();

    for (size_t sc = 0; sc < numClasses; ++sc) {
      salmonEqclasses[sc].count = 0;
    }

    for (size_t fn = 0; fn < totalNumFrags; ++fn) {
      salmonEqclasses[csamp(gen)].count += 1;
    }

    for (size_t i = 0; i < numGenes; ++i) {
      if ( initUniform ) {
        alphas[i] = 1.0 / numGenes;
      } else {
        alphas[i] = (geneAlphas[i] + 0.5) * 1e-3;
      }
    }

    bool converged{false};
    double maxRelDiff = -std::numeric_limits<double>::max();
    size_t itNum = 0;

    // EM termination criteria, adopted from Bray et al. 2016
    double minAlpha = 1e-8;
    double alphaCheckCutoff = 1e-2;
    constexpr double minWeight = std::numeric_limits<double>::denorm_min();

    while (itNum < minIter or (itNum < maxIter and !converged)) {
      CellEMUpdate_(salmonEqclasses, alphas, alphasPrime);

      converged = true;
      maxRelDiff = -std::numeric_limits<double>::max();
      for (size_t i = 0; i < numGenes; ++i) {
        if (alphasPrime[i] > alphaCheckCutoff) {
          double relDiff =
            std::abs(alphas[i] - alphasPrime[i]) / alphasPrime[i];
          maxRelDiff = (relDiff > maxRelDiff) ? relDiff : maxRelDiff;
          if (relDiff > relDiffTolerance) {
            converged = false;
          }
        }
        alphas[i] = alphasPrime[i];
        alphasPrime[i] = 0.0;
      }

      ++itNum;
    }//end-EM-while

    // Truncate tiny expression values
    double alphaSum = 0.0;
    // Truncate tiny expression values
    alphaSum = truncateAlphas(alphas, minAlpha);

    if (alphaSum < minWeight) {
      jointlog->error("Total alpha weight was too small! "
                      "Make sure you ran salmon correclty.");
      jointlog->flush();
      return false;
    }

    for(size_t i=0; i<numGenes; i++) {
      double alpha = alphas[i];
      mean[i] += alpha;
      squareMean[i] += alpha * alpha;
    }

    if (useAllBootstraps) {
      sampleEstimates.emplace_back(alphas);
    }
  }//end-boot-while

  // calculate mean and variance of the values
  for(size_t i=0; i<numGenes; i++) {
    double meanAlpha = mean[i] / numBootstraps;
    geneAlphas[i] = meanAlpha;
    variance[i] = (squareMean[i]/numBootstraps) - (meanAlpha*meanAlpha);
  }

  return true;
}

void optimizeCell(std::vector<std::string>& trueBarcodes,
                  const std::vector<std::vector<double>>& priorAlphas,
                  std::atomic<uint32_t>& barcode,
                  size_t totalCells, uint32_t umiEditDistance, eqMapT& eqMap,
                  std::deque<std::pair<TranscriptGroup, uint32_t>>& orderedTgroup,
                  std::shared_ptr<spdlog::logger>& jointlog,
                  std::vector<uint32_t>& umiCount,
                  std::vector<CellState>& skippedCB,
                  bool verbose, GZipWriter& gzw, bool noEM, bool useVBEM,
                  bool quiet, tbb::atomic<double>& totalDedupCounts,
                  tbb::atomic<uint32_t>& totalExpGeneCounts, double priorWeight,
                  spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                  uint32_t numGenes, uint32_t umiLength, uint32_t numBootstraps,
                  bool naiveEqclass, bool dumpUmiGraph, bool useAllBootstraps,
                  bool initUniform, CFreqMapT& freqCounter, bool dumpArborescences,
                  spp::sparse_hash_set<uint32_t>& mRnaGenes,
                  spp::sparse_hash_set<uint32_t>& rRnaGenes,
                  std::atomic<uint64_t>& totalUniEdgesCounts,
                  std::atomic<uint64_t>& totalBiEdgesCounts){
  size_t numCells {trueBarcodes.size()};
  size_t trueBarcodeIdx;

  // looping over until all the cells
  while((trueBarcodeIdx = barcode++) < totalCells) {
    // per-cell level optimization
    if ( umiCount[trueBarcodeIdx] == 0 ) {
      //skip the barcode if no mapped UMI
      skippedCB[trueBarcodeIdx].inActive = true;
      continue;
    }

    // extracting the sequence of the barcode
    auto& trueBarcodeStr = trueBarcodes[trueBarcodeIdx];

    CollapsedCellOptimizer::SerialVecType priorCellAlphas;
    if (useVBEM) {
      priorCellAlphas = priorAlphas[trueBarcodeIdx];
    }

    //extracting per-cell level eq class information
    double totalCount{0.0};
    double totalExpGenes{0};
    std::vector<uint32_t> eqIDs;
    std::vector<uint32_t> eqCounts;
    std::vector<UGroupT> umiGroups;
    std::vector<tgrouplabelt> txpGroups;
    std::vector<double> geneAlphas(numGenes, 0.0);
    std::vector<uint8_t> tiers (numGenes, 0);

    size_t fragmentCountValidator {0};
    for (auto& key : orderedTgroup) {
      //traversing each class and copying relevant data.
      bool isKeyPresent = eqMap.find_fn(key.first, [&](const SCTGValue& val){
          auto& bg = val.barcodeGroup;
          auto bcIt = bg.find(trueBarcodeIdx);

          // sub-selecting bgroup of this barcode only
          if (bcIt != bg.end()){
            // extracting txp labels
            const std::vector<uint32_t>& txps = key.first.txps;

            txpGroups.emplace_back(txps);
            umiGroups.emplace_back(bcIt->second);
            for(auto& ugroup: bcIt->second){
              fragmentCountValidator += ugroup.second;
            }

            // for dumping per-cell eqclass vector
            if(verbose){
              // original counts of the UMI
              uint32_t eqCount {0};
              for(auto& ugroup: bcIt->second){
                eqCount += ugroup.second;
              }

              eqIDs.push_back(static_cast<uint32_t>(key.second));
              eqCounts.push_back(eqCount);
            }
          }
        });

      if(!isKeyPresent){
        jointlog->error("Not able to find key in Cuckoo hash map."
                        "Please Report this issue on github");
        jointlog->flush();
        exit(1);
      }
    }

    if (fragmentCountValidator != umiCount[trueBarcodeIdx]) {
      jointlog->error("Feature count in feature dump doesn't map"
                      "with eqclasses frament counts\n");
      jointlog->flush();
      exit(1);
    }

    if ( !naiveEqclass ) {
      // perform the UMI deduplication step
      std::vector<SalmonEqClass> salmonEqclasses;
      std::vector<spp::sparse_hash_map<uint16_t, uint16_t>> arboEqClassCount;
      bool dedupOk = dedupClasses(geneAlphas, totalCount, txpGroups,
                                  umiGroups, salmonEqclasses, umiLength,
                                  txpToGeneMap, tiers, gzw, umiEditDistance,
                                  dumpUmiGraph, trueBarcodeStr,
                                  arboEqClassCount,
                                  dumpArborescences,
                                  totalUniEdgesCounts, totalBiEdgesCounts);

      if( !dedupOk ){
        jointlog->error("Deduplication for cell {} failed \n"
                        "Please Report this on github.", trueBarcodeStr);
        jointlog->flush();
        std::exit(74);
      }

      if ( numBootstraps and noEM ) {
        jointlog->error("Cannot perform bootstrapping with noEM");
        jointlog->flush();
        exit(1);
      }

      // perform EM for resolving ambiguity
      if ( !noEM ) {
        if ( useVBEM and not initUniform) {
          // down weighing priors for tier 2 estimates
          for (size_t j=0; j<numGenes; j++) {
            if (tiers[j] == 2) {
              priorCellAlphas[j] = priorWeight * 1e-2;
            }
          }
        }

        bool isEMok = runPerCellEM(totalCount,
                                   numGenes,
                                   geneAlphas,
                                   priorCellAlphas,
                                   salmonEqclasses,
                                   jointlog,
                                   initUniform,
                                   useVBEM);
        if( !isEMok ){
          jointlog->error("EM iteration for cell {} failed \n"
                          "Please Report this on github.", trueBarcodeStr);
          jointlog->flush();
          std::exit(74);
        }
      }

      std::string arboData;
      { // working out Arborescence level stats
        if (dumpArborescences) {
          size_t totalCellFrags {0};
          std::stringstream arboDataStream;

          std::vector<spp::sparse_hash_map<uint16_t, uint32_t>> arboFragCounts;
          arboFragCounts.resize(numGenes);

          for( size_t i=0; i<salmonEqclasses.size(); i++ ) {
            auto& eqclass = salmonEqclasses[i];
            auto& eqCounts = arboEqClassCount[i];
            size_t numLabels = eqclass.labels.size();

            for (auto gid: eqclass.labels) {
              if (gid >= numGenes) {
                std::cerr<< gid << "more than number of genes" << std::flush;
                exit(74);
              }
            }

            if ( numLabels == 1 ) {
              auto gid = eqclass.labels.front();
              for (auto it: eqCounts) {
                arboFragCounts[gid][it.first] += it.second;
                totalCellFrags += (it.first * it.second);
              }
            }
            else if ( numLabels > 1 ){

              // calculate the division probabilities
              std::vector<double> probs;
              for (auto gid: eqclass.labels) {
                probs.emplace_back( geneAlphas[gid] );
              }

              size_t arboId {0}, totalGeneFrags {0}, totalUmis{0};
              size_t numArbos = eqCounts.size();
              std::vector<uint16_t> arboLengths(numArbos);
              std::vector<uint16_t> arboCounts(numArbos);
              for (auto it: eqCounts) {
                arboLengths[arboId] = it.first;
                arboCounts[arboId] = it.second;

                arboId += 1;
                totalUmis += it.second;
                totalGeneFrags += (it.second * it.first);
              }

              std::discrete_distribution<> geneDist(probs.begin(), probs.end());
              std::discrete_distribution<> arboDist(arboCounts.begin(), arboCounts.end());

              std::mt19937 geneGen;
              std::mt19937 arboGen;

              for (size_t j=0; j < totalUmis; j++) {
                auto gid = eqclass.labels[geneDist(geneGen)];
                auto arboLength = arboLengths[arboDist(arboGen)];
                arboFragCounts[gid][arboLength] += 1;
              }

              totalCellFrags += totalGeneFrags;
            }
            else {
              std::cerr<<"Eqclasses with No gene labels\n" << std::flush;
              exit(74);
            }
          } //end-for

          // populating arbo txt file
          std::stringstream arboGeneData;
          size_t gid {0}, numExpGenes {0};
          for (auto& arboDist: arboFragCounts) {
            if (arboDist.size() > 0) {
              numExpGenes += 1;

              arboGeneData << gid << "\t" << arboDist.size() ;
              for(auto it: arboDist) {
                arboGeneData << "\t" << it.first << "\t" << it.second;
              }
              arboGeneData << std::endl;
            }

            gid += 1;
          } // done populating arboGeneData

          arboDataStream << trueBarcodeStr << "\t" << numExpGenes << "\t"
                         << totalCellFrags << "\n" << arboGeneData.str();
          arboData = arboDataStream.str();
        } //end-if
      } // end populatin arbo level stats

      std::string features;
      uint8_t featureCode {0};
      {
        std::stringstream featuresStream;
        featuresStream << trueBarcodeStr;

        // Making features
        double totalUmiCount {0.0};
        double maxNumUmi {0.0};
        for (auto count: geneAlphas) {
          if (count>0.0) {
            totalUmiCount += count;
            totalExpGenes += 1;
            if (count > maxNumUmi) { maxNumUmi = count; }
          }
        }

        uint32_t numGenesOverMean {0};
        double mitoCount {0.0}, riboCount {0.0};
        double meanNumUmi {totalUmiCount / totalExpGenes};
        double meanByMax = maxNumUmi ? meanNumUmi / maxNumUmi : 0.0;
        for (size_t j=0; j<geneAlphas.size(); j++){
          auto count = geneAlphas[j];
          if (count > meanNumUmi) { ++numGenesOverMean; }

          if (mRnaGenes.contains(j)){
            mitoCount += count;
          }
          if (rRnaGenes.contains(j)){
            riboCount += count;
          }
        }

        auto indexIt = freqCounter.find(trueBarcodeStr);
        bool indexOk = indexIt != freqCounter.end();
        if ( not indexOk ){
          jointlog->error("Error: index {} not found in freq Counter\n"
                          "Please Report the issue on github", trueBarcodeStr);
          jointlog->flush();
          exit(84);
        }

        uint64_t numRawReads = *indexIt;
        uint64_t numMappedReads { umiCount[trueBarcodeIdx] };
        double mappingRate = numRawReads ?
          numMappedReads / static_cast<double>(numRawReads) : 0.0;
        double deduplicationRate = numMappedReads ?
          1.0 - (totalUmiCount / numMappedReads) : 0.0;

        featuresStream << "\t" << numRawReads
                       << "\t" << numMappedReads
                       << "\t" << totalUmiCount
                       << "\t" << mappingRate
                       << "\t" << deduplicationRate
                       << "\t" << meanByMax
                       << "\t" << totalExpGenes
                       << "\t" << numGenesOverMean;

        if (mRnaGenes.size() > 1) {
          featureCode += 1;
          featuresStream << "\t" << mitoCount / totalUmiCount;
        }

        if (rRnaGenes.size() > 1) {
          featureCode += 2;
          featuresStream << "\t" << riboCount / totalUmiCount;
        }

        features = featuresStream.str();
      } // end making features

      // write the abundance for the cell
      bool isWriteOk = gzw.writeSparseAbundances( trueBarcodeStr,
                                                  features,
                                                  arboData,
                                                  featureCode,
                                                  geneAlphas,
                                                  tiers,
                                                  dumpArborescences,
                                                  dumpUmiGraph );

      if( not isWriteOk ){
        jointlog->error("Gzip Writer failed \n"
                        "Please Report this on github.");
        jointlog->flush();
        std::exit(74);
      }

      // maintaining count for total number of predicted UMI
      salmon::utils::incLoop(totalDedupCounts, totalCount);
      totalExpGeneCounts += totalExpGenes;

      if ( numBootstraps > 0 ){
        std::vector<std::vector<double>> sampleEstimates;
        std::vector<double> bootVariance(numGenes, 0.0);

        bool isBootstrappingOk = runBootstraps(numGenes,
                                               geneAlphas,
                                               salmonEqclasses,
                                               jointlog,
                                               numBootstraps,
                                               bootVariance,
                                               useAllBootstraps,
                                               sampleEstimates,
                                               initUniform);
        if( not isBootstrappingOk or
            (useAllBootstraps and sampleEstimates.size()!=numBootstraps)
            ){
          jointlog->error("Bootstrapping failed \n"
                          "Please Report this on github.");
          jointlog->flush();
          std::exit(74);
        }

        // write the abundance for the cell
        gzw.writeSparseBootstraps( trueBarcodeStr,
                                   geneAlphas, bootVariance,
                                   useAllBootstraps, sampleEstimates);
      }//end-if
    } else {
      // doing per eqclass level naive deduplication
      for (size_t eqId=0; eqId<umiGroups.size(); eqId++) {
        spp::sparse_hash_set<uint64_t> umis;

        for(auto& it: umiGroups[eqId]) {
          umis.insert( it.first );
        }
        totalCount += umis.size();

        // filling in the eqclass level deduplicated counts
        if (verbose) {
          eqCounts[eqId] = umis.size();
        }
      }

      // maintaining count for total number of predicted UMI
      salmon::utils::incLoop(totalDedupCounts, totalCount);
    }

    if (verbose) {
      gzw.writeCellEQVec(trueBarcodeIdx, eqIDs, eqCounts, true);
    }

    //printing on screen progress
    const char RESET_COLOR[] = "\x1b[0m";
    char green[] = "\x1b[30m";
    green[3] = '0' + static_cast<char>(fmt::GREEN);
    char red[] = "\x1b[30m";
    red[3] = '0' + static_cast<char>(fmt::RED);

    double cellCount {static_cast<double>(barcode)};//numCells-jqueue.size_approx()};
    if (cellCount > totalCells) { cellCount = totalCells; }
    double percentCompletion {cellCount*100/numCells};
    if (not quiet){
      fmt::print(stderr, "\033[A\r\r{}Analyzed {} cells ({}{}%{} of all).{}\n",
                 green, cellCount, red, round(percentCompletion), green, RESET_COLOR);
    }
  }
}

template <typename ProtocolT>
bool CollapsedCellOptimizer::optimize(EqMapT& fullEqMap,
                                      spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                                      spp::sparse_hash_map<std::string, uint32_t>& geneIdxMap,
                                      AlevinOpts<ProtocolT>& aopt,
                                      GZipWriter& gzw,
                                      std::vector<std::string>& trueBarcodes,
                                      std::vector<uint32_t>& umiCount,
                                      CFreqMapT& freqCounter,
                                      size_t numLowConfidentBarcode){
  size_t numCells = trueBarcodes.size();
  size_t numGenes = geneIdxMap.size();
  size_t numWorkerThreads{1};
  bool hasWhitelist = boost::filesystem::exists(aopt.whitelistFile);
  bool usingHashMode = boost::filesystem::exists(aopt.bfhFile);

  if (aopt.numThreads > 1) {
    numWorkerThreads = aopt.numThreads - 1;
  }

  //get the keys of the map
  std::deque<std::pair<TranscriptGroup, uint32_t>> orderedTgroup;
  //spp::sparse_hash_set<uint64_t> uniqueUmisCounter;
  uint32_t eqId{0};
  for(const auto& kv : fullEqMap.lock_table()){
    // assuming the iteration through lock table is always same
    if(kv.first.txps.size() == 1){
      orderedTgroup.push_front(std::make_pair(kv.first, eqId));
    }
    else{
      orderedTgroup.push_back(std::make_pair(kv.first, eqId));
    }
    eqId++;
  }

  if (aopt.noEM) {
    aopt.jointLog->warn("Not performing EM; this may result in discarding ambiguous reads\n");
    aopt.jointLog->flush();
  }

  if (aopt.initUniform) {
    if (aopt.useVBEM) {
      aopt.jointLog->warn("Using uniform initialization for VBEM");
    } else {
      aopt.jointLog->warn("Using uniform initialization for EM");
    }
    aopt.jointLog->flush();
  }

  spp::sparse_hash_set<uint32_t> mRnaGenes, rRnaGenes;
  bool useMito {false}, useRibo {false};
  if( not hasWhitelist ) {
    if (boost::filesystem::exists(aopt.mRnaFile)) {
      std::ifstream mRnaFile(aopt.mRnaFile.string());
      std::string gene;
      size_t skippedGenes {0};
      if(mRnaFile.is_open()) {
        while(getline(mRnaFile, gene)) {
          if (geneIdxMap.contains(gene)){
            mRnaGenes.insert(geneIdxMap[ gene ]);
          }
          else{
            skippedGenes += 1;
          }
        }
        mRnaFile.close();
      }
      if (skippedGenes > 0){
        aopt.jointLog->warn("{} mitorna gene(s) does not have transcript in the reference",
                            skippedGenes);
      }
      aopt.jointLog->info("Total {} usable mRna genes", mRnaGenes.size());
      if (mRnaGenes.size() > 0 ) { useMito = true; }
    } else if ( not usingHashMode ) {
      aopt.jointLog->warn("mrna file not provided; using is 1 less feature for whitelisting");
    }

    if(boost::filesystem::exists(aopt.rRnaFile)){
      std::ifstream rRnaFile(aopt.rRnaFile.string());
      std::string gene;
      size_t skippedGenes {0};
      if(rRnaFile.is_open()) {
        while(getline(rRnaFile, gene)) {
          if (geneIdxMap.contains(gene)){
            rRnaGenes.insert(geneIdxMap[ gene ]);
          }
          else{
            skippedGenes += 1;
          }
        }
        rRnaFile.close();
      }
      if (skippedGenes > 0){
        aopt.jointLog->warn("{} ribosomal rna gene(s) does not have transcript in the reference",
                            skippedGenes);
      }
      aopt.jointLog->info("Total {} usable rRna genes", rRnaGenes.size());
      if (rRnaGenes.size() > 0 ) { useRibo = true; }
    } else if ( not usingHashMode ) {
      aopt.jointLog->warn("rrna file not provided; using is 1 less feature for whitelisting");
    }
  } // done populating mRNA and rRNA genes

  double priorWeight {1.0};
  std::atomic<uint32_t> bcount{0};
  tbb::atomic<double> totalDedupCounts{0.0};
  tbb::atomic<uint32_t> totalExpGeneCounts{0};
  std::atomic<uint64_t> totalBiEdgesCounts{0};
  std::atomic<uint64_t> totalUniEdgesCounts{0};

  std::vector<CellState> skippedCB (numCells);
  std::vector<std::vector<double>> priorAlphas;
  if (aopt.useVBEM) {
    if (not aopt.initUniform) {
      auto path = boost::filesystem::path(aopt.vbemPriorFile);
      auto mfile = path / "quants_mat.csv";
      auto gfile = path / "quants_mat_cols.txt";
      auto cfile = path / "quants_mat_rows.txt";

      std::vector<std::string> cnames;
      if(boost::filesystem::exists(cfile)){
        std::ifstream fileReader(cfile.string());
        std::string data;
        if(fileReader.is_open()) {
          while(getline(fileReader, data)) {
            cnames.emplace_back(data);
          }
          fileReader.close();
        }
        aopt.jointLog->info("Done importing Cellular Barcodes for Prior w/ {} cbs",
                            cnames.size());
      } else {
        aopt.jointLog->error("prior file (quants_mat_rows.txt): {} doesn't exist",
                             cfile.string());
        aopt.jointLog->flush();
        std::exit(84);
      }

      std::vector<std::string> gnames;
      if(boost::filesystem::exists(gfile)){
        std::ifstream fileReader(gfile.string());
        std::string data;
        if(fileReader.is_open()) {
          while(getline(fileReader, data)) {
            gnames.emplace_back(data);
          }
          fileReader.close();
        }

        size_t priorOnlyGenes {0};
        for (auto& gname: gnames) {
          if (!geneIdxMap.contains(gname)) {
            priorOnlyGenes += 1;
            //aopt.jointLog->warn("prior file has gene {} not in txp2gene map",
            //                     gname);
            //aopt.jointLog->flush();
            //std::exit(84);
          }
        }

        if (priorOnlyGenes > 0) {
          aopt.jointLog->warn("prior file has {} genes not in txp2gene map",
                              priorOnlyGenes);
          aopt.jointLog->flush();
        }
        aopt.jointLog->info("Done importing Gene names for Prior w/ {} genes",
                            gnames.size());
      } else {
        aopt.jointLog->error("prior file (quants_mat_cols.txt): {} doesn't exist",
                             cfile.string());
        aopt.jointLog->flush();
        std::exit(84);
      }

      double priorMolCounts {0.0};
      { // starting reading prior matrix
        if(boost::filesystem::exists(mfile)){
          std::ifstream fileReader(mfile.string());
          std::string data;

          if(fileReader.is_open()) {
            while(getline(fileReader, data)) {
              CollapsedCellOptimizer::SerialVecType cellCount(gnames.size(), 0.0);
              std::stringstream ss(data);

              size_t idxPtr {0};
              while( ss.good() ) {
                std::string substr;
                getline( ss, substr, ',' );
                double count = std::stod(substr);

                priorMolCounts += count;
                cellCount[idxPtr] = count;
                if ( ++idxPtr == gnames.size() ) { break; }
              }//end-while

              if (cellCount.size() != gnames.size()) {
                aopt.jointLog->error("Incomplete Prior File");
                aopt.jointLog->flush();
                std::exit(84);
              }
              priorAlphas.emplace_back(cellCount);
            }//end-outer-while
            fileReader.close();
          }
          aopt.jointLog->info("Done importing Matrix for Prior of {} X {}",
                              priorAlphas.size(), priorAlphas[0].size());
        }
      }//end-matrix reading scope

      priorWeight = aopt.vbemNorm / priorMolCounts ;
      aopt.jointLog->info( "Prior Weight: {}/ {}", priorWeight, priorMolCounts);
      {
        //rearragngement of vectors
        size_t noPriorCellCount {0};
        std::vector<std::vector<double>> temps(trueBarcodes.size(), std::vector<double>(numGenes, priorWeight * 1e-2));
        for (size_t i=0; i<trueBarcodes.size(); i++) {
          auto& cname = trueBarcodes[i];
          auto it = std::find(cnames.begin(), cnames.end(), cname);
          if (it != cnames.end()) {
            size_t cIdx = distance(cnames.begin(), it);
            for (size_t j=0; j<gnames.size(); j++) {
              auto& gname = gnames[j];
              if (priorAlphas[cIdx][j] > 0 && geneIdxMap.contains(gname)) {
                uint32_t gIdx = geneIdxMap[gname];
                temps[i][gIdx] += priorWeight * priorAlphas[cIdx][j];
              }
            }
          } else {
            noPriorCellCount += 1;
          }
        } //end-for

        priorAlphas = temps;

        aopt.jointLog->info("Done Rearranging Matrix for Prior of {} X {}",
                            priorAlphas.size(), priorAlphas[0].size());
        if (noPriorCellCount > 0) {
          aopt.jointLog->warn("Can't find prior and using uniform priors for {} cells", noPriorCellCount);
        }
        aopt.jointLog->flush();
      } // end-rearrangment
    } else { //end-else not initUniform
      priorAlphas = std::vector<std::vector<double>> (numCells, std::vector<double>(numGenes, 1e-2) );
    }
  }//end-if useVBEM

  std::vector<std::thread> workerThreads;
  for (size_t tn = 0; tn < numWorkerThreads; ++tn) {
    workerThreads.emplace_back(optimizeCell,
                               std::ref(trueBarcodes),
                               std::ref(priorAlphas),
                               std::ref(bcount),
                               numCells,
                               aopt.umiEditDistance,
                               std::ref(fullEqMap),
                               std::ref(orderedTgroup),
                               std::ref(aopt.jointLog),
                               std::ref(umiCount),
                               std::ref(skippedCB),
                               aopt.dumpBarcodeEq,
                               std::ref(gzw),
                               aopt.noEM,
                               aopt.useVBEM,
                               aopt.quiet,
                               std::ref(totalDedupCounts),
                               std::ref(totalExpGeneCounts),
                               priorWeight,
                               std::ref(txpToGeneMap),
                               numGenes,
                               aopt.protocol.umiLength,
                               aopt.numBootstraps,
                               aopt.naiveEqclass,
                               aopt.dumpUmiGraph,
                               aopt.dumpfeatures,
                               aopt.initUniform,
                               std::ref(freqCounter),
                               aopt.dumpArborescences,
                               std::ref(rRnaGenes),
                               std::ref(mRnaGenes),
                               std::ref(totalUniEdgesCounts),
                               std::ref(totalBiEdgesCounts));
  }

  for (auto& t : workerThreads) {
    t.join();
  }
  aopt.jointLog->info("Total {0:.2f} UMI after deduplicating.",
                      totalDedupCounts);
  aopt.jointLog->info("Total {} BiDirected Edges.",
                      totalBiEdgesCounts);
  aopt.jointLog->info("Total {} UniDirected Edges.",
                      totalUniEdgesCounts);

  //adjusting for float
  aopt.totalDedupUMIs = totalDedupCounts+1;
  aopt.totalExpGenes = totalExpGeneCounts;

  uint32_t skippedCBcount {0};
  for(auto cb: skippedCB){
    if (cb.inActive) {
      skippedCBcount += 1;
    }
  }

  if( skippedCBcount > 0 ) {
    aopt.jointLog->warn("Skipped {} barcodes due to No mapped read",
                        skippedCBcount);
    auto lowRegionCutoffIdx = numCells - numLowConfidentBarcode;
    std::vector<std::string> retainedTrueBarcodes ;
    for (size_t idx=0; idx < numCells; idx++){
      // not very efficient way but assuming the size is small enough
      if (skippedCB[idx].inActive) {
        if (idx > lowRegionCutoffIdx){
          numLowConfidentBarcode--;
        }
      } else {
        retainedTrueBarcodes.emplace_back(trueBarcodes[idx]);
      }
    }

    trueBarcodes = retainedTrueBarcodes;
    numCells = trueBarcodes.size();
  }

  gzw.close_all_streams();

  std::vector<std::string> geneNames(numGenes);
  for (auto geneIdx : geneIdxMap) {
    geneNames[geneIdx.second] = geneIdx.first;
  }
  boost::filesystem::path gFilePath = aopt.outputDirectory / "quants_mat_cols.txt";
  std::ofstream gFile(gFilePath.string());
  std::ostream_iterator<std::string> giterator(gFile, "\n");
  std::copy(geneNames.begin(), geneNames.end(), giterator);
  gFile.close();

  if( not hasWhitelist and not usingHashMode){
    aopt.jointLog->info("Clearing EqMap; Might take some time.");
    fullEqMap.clear();

    if ( numLowConfidentBarcode < aopt.lowRegionMinNumBarcodes ) {
      aopt.jointLog->warn("Num Low confidence barcodes too less {} < {}."
                          "Can't performing whitelisting; Skipping",
                          numLowConfidentBarcode,
                          aopt.lowRegionMinNumBarcodes);

    } else if (trueBarcodes.size() - numLowConfidentBarcode < 90) {
        aopt.jointLog->warn("Num High confidence barcodes too less {} < 90."
                            "Can't performing whitelisting; Skipping",
                            trueBarcodes.size() - numLowConfidentBarcode);
    } else {
      aopt.jointLog->info("Starting white listing of {} cells", trueBarcodes.size());
      bool whitelistingSuccess = alevin::whitelist::performWhitelisting(aopt,
                                                                        umiCount,
                                                                        trueBarcodes,
                                                                        freqCounter,
                                                                        useRibo,
                                                                        useMito,
                                                                        numLowConfidentBarcode);
      if (!whitelistingSuccess) {
        aopt.jointLog->error(
                             "The white listing algorithm failed. This is likely the result of "
                             "bad input (or a bug). If you cannot track down the cause, please "
                             "report this issue on GitHub.");
        aopt.jointLog->flush();
        return false;
      }

      aopt.jointLog->info("Finished white listing");
      aopt.jointLog->flush();
    }
  } //end-if whitelisting
  else if ( usingHashMode and not hasWhitelist ) {
    aopt.jointLog->warn("intelligent whitelisting is disabled in hash mode; skipping");
    aopt.jointLog->flush();
  }

  if (aopt.dumpMtx){
    aopt.jointLog->info("Starting dumping cell v gene counts in mtx format");
    boost::filesystem::path qFilePath = aopt.outputDirectory / "quants_mat.mtx.gz";

    boost::iostreams::filtering_ostream qFile;
    qFile.push(boost::iostreams::gzip_compressor(6));
    qFile.push(boost::iostreams::file_sink(qFilePath.string(),
                                           std::ios_base::out | std::ios_base::binary));

    // mtx header
    qFile << "%%MatrixMarket\tmatrix\tcoordinate\treal\tgeneral" << std::endl
          << numCells << "\t" << numGenes << "\t" << totalExpGeneCounts << std::endl;

    {

      auto popcount = [](uint8_t n) {
        size_t count {0};
        while (n) {
          n &= n-1;
          ++count;
        }
        return count;
      };

      uint32_t zerod_cells {0};
      size_t numFlags = std::ceil(numGenes/8.0);
      std::vector<uint8_t> alphasFlag (numFlags, 0);
      size_t flagSize = sizeof(decltype(alphasFlag)::value_type);

      std::vector<float> alphasSparse;
      alphasSparse.reserve(numFlags/2);
      size_t elSize = sizeof(decltype(alphasSparse)::value_type);

      auto countMatFilename = aopt.outputDirectory / "quants_mat.gz";
      if(not boost::filesystem::exists(countMatFilename)){
        std::cout<<"ERROR: Can't import Binary file quants.mat.gz, it doesn't exist" << std::flush;
        exit(84);
      }

      boost::iostreams::filtering_istream countMatrixStream;
      countMatrixStream.push(boost::iostreams::gzip_decompressor());
      countMatrixStream.push(boost::iostreams::file_source(countMatFilename.string(),
                                                           std::ios_base::in | std::ios_base::binary));

      for (size_t cellCount=0; cellCount<numCells; cellCount++){
        countMatrixStream.read(reinterpret_cast<char*>(alphasFlag.data()), flagSize * numFlags);

        size_t numExpGenes {0};
        std::vector<size_t> indices;
        for (size_t j=0; j<alphasFlag.size(); j++) {
          uint8_t flag = alphasFlag[j];
          size_t numNonZeros = popcount(flag);
          numExpGenes += numNonZeros;

          for (size_t i=0; i<8; i++){
            if (flag & (128 >> i)) {
              indices.emplace_back( i+(8*j) );
            }
          }
        }

        if (indices.size() != numExpGenes) {
          aopt.jointLog->error("binary format reading error {}: {}: {}",
                               indices.size(), numExpGenes);
          aopt.jointLog->flush();
          exit(84);
        }


        alphasSparse.clear();
        alphasSparse.resize(numExpGenes);
        countMatrixStream.read(reinterpret_cast<char*>(alphasSparse.data()), elSize * numExpGenes);

        float readCount {0.0};
        readCount += std::accumulate(alphasSparse.begin(), alphasSparse.end(), 0.0);
        if (readCount > 1000000) {
          aopt.jointLog->warn("A cell has more 1M count, Possible error");
          aopt.jointLog->flush();
        }

        for(size_t i=0; i<numExpGenes; i++) {
          qFile << std::fixed
                << cellCount + 1 << "\t"
                << indices[i] + 1 << "\t"
                << alphasSparse[i] <<  std::endl;
        }

        if (readCount == 0.0){
          zerod_cells += 1;
        }
      } // end-for each cell

      if (zerod_cells > 0) {
        aopt.jointLog->warn("Found {} cells with 0 counts", zerod_cells);
      }
    }

    boost::iostreams::close(qFile);
    aopt.jointLog->info("Finished dumping counts into mtx");
  }

  return true;
} //end-optimize


namespace apt = alevin::protocols;
template
bool CollapsedCellOptimizer::optimize(EqMapT& fullEqMap,
                                      spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                                      spp::sparse_hash_map<std::string, uint32_t>& geneIdxMap,
                                      AlevinOpts<apt::DropSeq>& aopt,
                                      GZipWriter& gzw,
                                      std::vector<std::string>& trueBarcodes,
                                      std::vector<uint32_t>& umiCount,
                                      CFreqMapT& freqCounter,
                                      size_t numLowConfidentBarcode);
template
bool CollapsedCellOptimizer::optimize(EqMapT& fullEqMap,
                                      spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                                      spp::sparse_hash_map<std::string, uint32_t>& geneIdxMap,
                                      AlevinOpts<apt::CITESeq>& aopt,
                                      GZipWriter& gzw,
                                      std::vector<std::string>& trueBarcodes,
                                      std::vector<uint32_t>& umiCount,
                                      CFreqMapT& freqCounter,
                                      size_t numLowConfidentBarcode);
template
bool CollapsedCellOptimizer::optimize(EqMapT& fullEqMap,
                                      spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                                      spp::sparse_hash_map<std::string, uint32_t>& geneIdxMap,
                                      AlevinOpts<apt::InDrop>& aopt,
                                      GZipWriter& gzw,
                                      std::vector<std::string>& trueBarcodes,
                                      std::vector<uint32_t>& umiCount,
                                      CFreqMapT& freqCounter,
                                      size_t numLowConfidentBarcode);
template
bool CollapsedCellOptimizer::optimize(EqMapT& fullEqMap,
                                      spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                                      spp::sparse_hash_map<std::string, uint32_t>& geneIdxMap,
                                      AlevinOpts<apt::ChromiumV3>& aopt,
                                      GZipWriter& gzw,
                                      std::vector<std::string>& trueBarcodes,
                                      std::vector<uint32_t>& umiCount,
                                      CFreqMapT& freqCounter,
                                      size_t numLowConfidentBarcode);
template
bool CollapsedCellOptimizer::optimize(EqMapT& fullEqMap,
                                      spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                                      spp::sparse_hash_map<std::string, uint32_t>& geneIdxMap,
                                      AlevinOpts<apt::Chromium>& aopt,
                                      GZipWriter& gzw,
                                      std::vector<std::string>& trueBarcodes,
                                      std::vector<uint32_t>& umiCount,
                                      CFreqMapT& freqCounter,
                                      size_t numLowConfidentBarcode);
template
bool CollapsedCellOptimizer::optimize(EqMapT& fullEqMap,
                                      spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                                      spp::sparse_hash_map<std::string, uint32_t>& geneIdxMap,
                                      AlevinOpts<apt::Gemcode>& aopt,
                                      GZipWriter& gzw,
                                      std::vector<std::string>& trueBarcodes,
                                      std::vector<uint32_t>& umiCount,
                                      CFreqMapT& freqCounter,
                                      size_t numLowConfidentBarcode);
template
bool CollapsedCellOptimizer::optimize(EqMapT& fullEqMap,
                                      spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                                      spp::sparse_hash_map<std::string, uint32_t>& geneIdxMap,
                                      AlevinOpts<apt::CELSeq>& aopt,
                                      GZipWriter& gzw,
                                      std::vector<std::string>& trueBarcodes,
                                      std::vector<uint32_t>& umiCount,
                                      CFreqMapT& freqCounter,
                                      size_t numLowConfidentBarcode);
template
bool CollapsedCellOptimizer::optimize(EqMapT& fullEqMap,
                                      spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                                      spp::sparse_hash_map<std::string, uint32_t>& geneIdxMap,
                                      AlevinOpts<apt::CELSeq2>& aopt,
                                      GZipWriter& gzw,
                                      std::vector<std::string>& trueBarcodes,
                                      std::vector<uint32_t>& umiCount,
                                      CFreqMapT& freqCounter,
                                      size_t numLowConfidentBarcode);
template
bool CollapsedCellOptimizer::optimize(EqMapT& fullEqMap,
                                      spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                                      spp::sparse_hash_map<std::string, uint32_t>& geneIdxMap,
                                      AlevinOpts<apt::QuartzSeq2>& aopt,
                                      GZipWriter& gzw,
                                      std::vector<std::string>& trueBarcodes,
                                      std::vector<uint32_t>& umiCount,
                                      CFreqMapT& freqCounter,
                                      size_t numLowConfidentBarcode);

template
bool CollapsedCellOptimizer::optimize(EqMapT& fullEqMap,
                                      spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                                      spp::sparse_hash_map<std::string, uint32_t>& geneIdxMap,
                                      AlevinOpts<apt::Custom>& aopt,
                                      GZipWriter& gzw,
                                      std::vector<std::string>& trueBarcodes,
                                      std::vector<uint32_t>& umiCount,
                                      CFreqMapT& freqCounter,
                                      size_t numLowConfidentBarcode);
