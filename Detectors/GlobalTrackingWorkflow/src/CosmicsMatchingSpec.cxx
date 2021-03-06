// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// @file   CosmicsMatchingSpec.cxx

#include <vector>

#include "Framework/ConfigParamRegistry.h"
#include "GlobalTrackingWorkflow/CosmicsMatchingSpec.h"
#include "ReconstructionDataFormats/GlobalTrackAccessor.h"
#include "ReconstructionDataFormats/GlobalTrackID.h"
#include "ReconstructionDataFormats/TrackTPCITS.h"
#include "ReconstructionDataFormats/MatchInfoTOF.h"
#include "ReconstructionDataFormats/TrackTPCTOF.h"
#include "SimulationDataFormat/MCCompLabel.h"
#include "SimulationDataFormat/MCTruthContainer.h"
#include "DataFormatsITS/TrackITS.h"
#include "DataFormatsITSMFT/CompCluster.h"
#include "DataFormatsITSMFT/ROFRecord.h"
#include "DataFormatsITSMFT/TopologyDictionary.h"
#include "DataFormatsTPC/TrackTPC.h"
#include "DataFormatsTPC/ClusterNative.h"
#include "DataFormatsTPC/WorkflowHelper.h"
#include "DetectorsBase/GeometryManager.h"
#include "DetectorsBase/Propagator.h"
#include "DetectorsCommonDataFormats/NameConf.h"
#include "DataFormatsParameters/GRPObject.h"
#include "Headers/DataHeader.h"
#include "CommonDataFormat/InteractionRecord.h"
#include "ITSBase/GeometryTGeo.h"
#include "ITSMFTBase/DPLAlpideParam.h"
#include "GlobalTracking/RecoContainer.h"

// RSTODO to remove once the framework will start propagating the header.firstTForbit
#include "DetectorsRaw/HBFUtils.h"

using namespace o2::framework;
using MCLabelsTr = gsl::span<const o2::MCCompLabel>;
using GTrackID = o2::dataformats::GlobalTrackID;
using DetID = o2::detectors::DetID;

namespace o2
{
namespace globaltracking
{

DataRequest dataRequest;

void CosmicsMatchingSpec::init(InitContext& ic)
{
  mTimer.Stop();
  mTimer.Reset();
  //-------- init geometry and field --------//
  o2::base::GeometryManager::loadGeometry();
  o2::base::Propagator::initFieldFromGRP(o2::base::NameConf::getGRPFileName());
  std::unique_ptr<o2::parameters::GRPObject> grp{o2::parameters::GRPObject::loadFrom(o2::base::NameConf::getGRPFileName())};
  const auto& alpParams = o2::itsmft::DPLAlpideParam<DetID::ITS>::Instance();
  if (!grp->isDetContinuousReadOut(DetID::ITS)) {
    mMatching.setITSROFrameLengthMUS(alpParams.roFrameLengthTrig / 1.e3); // ITS ROFrame duration in \mus
  } else {
    mMatching.setITSROFrameLengthMUS(alpParams.roFrameLengthInBC * o2::constants::lhc::LHCBunchSpacingNS * 1e-3); // ITS ROFrame duration in \mus
  }
  //
  std::string dictPath = ic.options().get<std::string>("its-dictionary-path");
  std::string dictFile = o2::base::NameConf::getDictionaryFileName(DetID::ITS, dictPath, ".bin");
  auto itsDict = std::make_unique<o2::itsmft::TopologyDictionary>();
  if (o2::base::NameConf::pathExists(dictFile)) {
    itsDict->readBinaryFile(dictFile);
    LOG(INFO) << "Matching is running with a provided ITS dictionary: " << dictFile;
  } else {
    LOG(INFO) << "Dictionary " << dictFile << " is absent, Matching expects ITS cluster patterns";
  }
  o2::its::GeometryTGeo::Instance()->fillMatrixCache(o2::math_utils::bit2Mask(o2::math_utils::TransformType::T2GRot) | o2::math_utils::bit2Mask(o2::math_utils::TransformType::T2L));
  mMatching.setITSDict(itsDict);

  // this is a hack to provide Mat.LUT from the local file, in general will be provided by the framework from CCDB
  std::string matLUTPath = ic.options().get<std::string>("material-lut-path");
  std::string matLUTFile = o2::base::NameConf::getMatLUTFileName(matLUTPath);
  if (o2::base::NameConf::pathExists(matLUTFile)) {
    auto* lut = o2::base::MatLayerCylSet::loadFromFile(matLUTFile);
    o2::base::Propagator::Instance()->setMatLUT(lut);
    LOG(INFO) << "Loaded material LUT from " << matLUTFile;
  } else {
    LOG(INFO) << "Material LUT " << matLUTFile << " file is absent, only TGeo can be used";
  }
  mMatching.setUseMC(mUseMC);
  mMatching.init();
  //
}

void CosmicsMatchingSpec::run(ProcessingContext& pc)
{
  mTimer.Start(false);

  RecoContainer recoData;
  recoData.collectData(pc, dataRequest);
  mMatching.process(recoData);
  pc.outputs().snapshot(Output{"GLO", "COSMICTRC", 0, Lifetime::Timeframe}, mMatching.getCosmicTracks());
  if (mUseMC) {
    pc.outputs().snapshot(Output{"GLO", "COSMICTRC_MC", 0, Lifetime::Timeframe}, mMatching.getCosmicTracksLbl());
  }
  mTimer.Stop();
}

void CosmicsMatchingSpec::endOfStream(EndOfStreamContext& ec)
{
  LOGF(INFO, "Cosmics matching total timing: Cpu: %.3e Real: %.3e s in %d slots",
       mTimer.CpuTime(), mTimer.RealTime(), mTimer.Counter() - 1);
}

DataProcessorSpec getCosmicsMatchingSpec(DetID::mask_t dets, bool useMC)
{

  std::vector<InputSpec> inputs;
  std::vector<OutputSpec> outputs;
  if (dets[DetID::ITS]) {
    dataRequest.requestITSTracks(useMC);
    dataRequest.requestITSClusters(false);
  }
  if (dets[DetID::TPC]) {
    dataRequest.requestTPCTracks(useMC);
    dataRequest.requestTPCClusters(false);
  }
  if (dets[DetID::ITS] && dets[DetID::TPC]) {
    dataRequest.requestITSTPCTracks(useMC);
  }
  if (dets[DetID::TPC] && dets[DetID::TOF]) {
    dataRequest.requestTPCTOFTracks(useMC);
    dataRequest.requestTOFClusters(false); // RSTODO Needed just to set the time of ITSTPC track, consider moving to MatchInfoTOF
    if (dets[DetID::ITS]) {
      dataRequest.requestTOFMatches(useMC);
    }
  }

  outputs.emplace_back("GLO", "COSMICTRC", 0, Lifetime::Timeframe);
  if (useMC) {
    outputs.emplace_back("GLO", "COSMICTRC_MC", 0, Lifetime::Timeframe);
  }

  return DataProcessorSpec{
    "cosmics-matcher",
    dataRequest.inputs,
    outputs,
    AlgorithmSpec{adaptFromTask<CosmicsMatchingSpec>(useMC)},
    Options{
      {"its-dictionary-path", VariantType::String, "", {"Path of the cluster-topology dictionary file"}},
      {"material-lut-path", VariantType::String, "", {"Path of the material LUT file"}}}};
}

} // namespace globaltracking
} // namespace o2
