/**
 * \file PHTpcCentralMembraneMatcher.cc
 * \brief match reconstructed CM clusters to CM pads, calculate differences, store on the node tree and compute distortion reconstruction maps
 * \author Tony Frawley <frawley@fsunuc.physics.fsu.edu>, Hugo Pereira Da Costa <hugo.pereira-da-costa@cea.fr>
 */

#include "PHTpcCentralMembraneMatcher.h"

#include <fun4all/Fun4AllReturnCodes.h>

#include <phool/PHCompositeNode.h>
#include <phool/getClass.h>
#include <phool/phool.h>
#include <trackbase/CMFlashClusterv2.h>
#include <trackbase/CMFlashClusterContainerv1.h>
#include <trackbase/CMFlashDifferencev1.h>
#include <trackbase/CMFlashDifferenceContainerv1.h>

#include <TCanvas.h>
#include <TFile.h>
#include <TGraph.h>
#include <TH1F.h>
#include <TH1D.h>
#include <TH2F.h>
#include <TF1.h>
#include <TNtuple.h>
#include <TString.h>
#include <TVector3.h>
#include <TStyle.h>

#include <cmath>
#include <set>
#include <string>

namespace 
{
  template<class T> inline constexpr T delta_phi(const T& phi)
  {
    if (phi > M_PI) return phi - 2. * M_PI;
    else if (phi <= -M_PI) return phi + 2.* M_PI;
    else return phi;
  }
  
  template<class T> inline constexpr T square( const T& x ) { return x*x; }
  
  template<class T> inline T get_r( const T& x, const T& y ) { return std::sqrt( square(x) + square(y) ); }
  
  // stream acts vector3
  [[maybe_unused]] std::ostream& operator << (std::ostream& out, const Acts::Vector3& v )
  { 
    out << "(" << v.x() << ", " << v.y() << ", " << v.z() << ")";
    return out;
  }

  /// normalize distortions based on the number of entries in each cell, as recorded in the m_hentries histogram
  [[maybe_unused]] void normalize_distortions( TpcDistortionCorrectionContainer* dcc )
  {
    // loop over side
    for( unsigned int i = 0; i<2; ++i )
    {

      // loop over bins in entries
      for( int ip = 0; ip < dcc->m_hentries[i]->GetNbinsX(); ++ip )
        for( int ir = 0; ir < dcc->m_hentries[i]->GetNbinsY(); ++ir )
      {
        // count number of times a given cell was filled
        const auto entries = dcc->m_hentries[i]->GetBinContent( ip+1, ir+1 );
        if( entries <= 1 ) continue;

        // normalize histograms
        for( const auto& h:{dcc->m_hDRint[i], dcc->m_hDPint[i], dcc->m_hDZint[i]} )
        {
          h->SetBinContent( ip+1, ir+1, h->GetBinContent( ip+1, ir+1 )/entries );
          h->SetBinError( ip+1, ir+1, h->GetBinError( ip+1, ir+1 )/entries );
        }
      }
    }
  }

  /// fill distortion correction histograms' guarding bins, to allow ::Interpolate to work over the full acceptance
  [[maybe_unused]] void fill_guarding_bins( TpcDistortionCorrectionContainer* dcc )
  {

    // loop over side
    for( unsigned int i = 0; i<2; ++i )
    {
      for( const auto& h:{dcc->m_hDRint[i], dcc->m_hDPint[i], dcc->m_hDZint[i], dcc->m_hentries[i]} )
      {

        // fill guarding phi bins
        /*
        * we use 2pi periodicity to do that:
        * - last valid bin is copied to first guarding bin;
        * - first valid bin is copied to last guarding bin
        */
        const auto phibins = h->GetNbinsX();
        const auto rbins = h->GetNbinsY();
        for( int ir = 0; ir < rbins; ++ir )
        {
          // copy last valid bin to first guarding bin
          h->SetBinContent( 1, ir+1, h->GetBinContent( phibins-1, ir+1 ) );
          h->SetBinError( 1, ir+1, h->GetBinError( phibins-1, ir+1 ) );

          // copy first valid bin to last guarding bin
          h->SetBinContent( phibins, ir+1, h->GetBinContent( 2, ir+1 ) );
          h->SetBinError( phibins, ir+1, h->GetBinError( 2, ir+1 ) );
        }

        // fill guarding r bins
        for( int iphi = 0; iphi < phibins; ++iphi )
        {
          // copy first valid bin to first guarding bin
          h->SetBinContent( iphi+1, 1, h->GetBinContent( iphi+1, 2 ) );
          h->SetBinError( iphi+1, 1, h->GetBinError( iphi+1, 2 ) );

          // copy last valid bin to last guarding bin
          h->SetBinContent( iphi+1, rbins, h->GetBinContent( iphi+1, rbins-1 ) );
          h->SetBinError( iphi+1, rbins, h->GetBinError( iphi+1, rbins-1 ) );
        }
      }
    }
  }

}

//____________________________________________________________________________..
PHTpcCentralMembraneMatcher::PHTpcCentralMembraneMatcher(const std::string &name):
  SubsysReco(name)
{
  // calculate stripes center positions
  CalculateCenters(nPads_R1, R1_e, nGoodStripes_R1_e, keepUntil_R1_e, nStripesIn_R1_e, nStripesBefore_R1_e, cx1_e, cy1_e);
  CalculateCenters(nPads_R1, R1, nGoodStripes_R1, keepUntil_R1, nStripesIn_R1, nStripesBefore_R1, cx1, cy1);
  CalculateCenters(nPads_R2, R2, nGoodStripes_R2, keepUntil_R2, nStripesIn_R2, nStripesBefore_R2, cx2, cy2);
  CalculateCenters(nPads_R3, R3, nGoodStripes_R3, keepUntil_R3, nStripesIn_R3, nStripesBefore_R3, cx3, cy3);
}


//___________________________________________________________
void PHTpcCentralMembraneMatcher::set_grid_dimensions( int phibins, int rbins )
{
  m_phibins = phibins;
  m_rbins = rbins;
}

/*
std::vector<double> PHTpcCentralMembraneMatcher::getRGaps( TH2F *r_phi ){
  
  TH1D *proj = r_phi->ProjectionY("R_proj",1,360);

  std::vector<double> pass1;

  for(int i=2; i<proj->GetNbinsX(); i++){
    if(proj->GetBinContent(i) > 0.15*proj->GetMaximum() && proj->GetBinContent(i) >= proj->GetBinContent(i-1) && proj->GetBinContent(i) >= proj->GetBinContent(i+1)) pass1.push_back(proj->GetBinCenter(i));
  }

  for(int i=0; i<(int)pass1.size()-1; i++){
    if(pass1[i+1]-pass1[i] > 0.75) continue;
    
    if(proj->GetBinContent(proj->FindBin(pass1[i])) > proj->GetBinContent(proj->FindBin(pass1[i+1]))) pass1.erase(std::next(pass1.begin(), i+1));
    else pass1.erase(std::next(pass1.begin(), i));

    i--;
  }

  return pass1;

}
*/

double PHTpcCentralMembraneMatcher::getPhiRotation_smoothed(TH1D *hitHist, TH1D *clustHist){

  TCanvas *c1 = new TCanvas();
  
  gStyle->SetOptFit(1);

  hitHist->Smooth();
  //  clustHist->Smooth();

  TF1 *f1 = new TF1("f1",[&](double *x, double *p){ return p[0] * hitHist->GetBinContent(hitHist->FindBin(x[0] - p[1])); }, -M_PI, M_PI, 2);
  f1->SetParNames("A","shift");
  f1->SetParameters(1.0,0.0);
  //  f1->SetParLimits(1,-M_PI/18,M_PI/18);

  clustHist->Fit("f1","IL");

  clustHist->Draw();
  f1->Draw("same");

  c1->SaveAs(Form("%s_fit.png",clustHist->GetName()));

  gStyle->SetOptFit(0);

  return f1->GetParameter(1);
}

std::vector<std::vector<double>> PHTpcCentralMembraneMatcher::getPhiGaps( TH2F *r_phi ){
  
  int bin0 = r_phi->GetYaxis()->FindBin(0.0);
  int bin40 = r_phi->GetYaxis()->FindBin(40.0);
  int bin58 = r_phi->GetYaxis()->FindBin(58.0);
  int bin100 = r_phi->GetYaxis()->FindBin(99.99);
  
  TH1D *phiHist[3];
  phiHist[0] = r_phi->ProjectionX("phiHist_R1",bin0,bin40);
  phiHist[1] = r_phi->ProjectionX("phiHist_R2",bin40,bin58);
  phiHist[2] = r_phi->ProjectionX("phiHist_R3",bin58,bin100);

  std::vector<std::vector<double>> phiGaps;

  for(int R=0; R<3; R++){
    std::vector<double> phiGaps_R;
    for(int i=2; i<=phiHist[R]->GetNbinsX(); i++){
      if(phiHist[R]->GetBinContent(i) > 0 && phiHist[R]->GetBinContent(i-1) == 0){
	if(phiGaps_R.size() == 0) phiGaps_R.push_back(phiHist[R]->GetBinCenter(i));
	else if(phiHist[R]->GetBinCenter(i) - phiGaps_R[phiGaps_R.size()-1] > (M_PI/36.)) phiGaps_R.push_back(phiHist[R]->GetBinCenter(i));
      }
    }

    phiGaps.push_back(phiGaps_R);
  }

  return phiGaps;

}

std::vector<double> PHTpcCentralMembraneMatcher::getAverageRotation(std::vector<std::vector<double>> hit, std::vector<std::vector<double>> clust){

  std::vector<double> avgAngle;

  for(int R=0; R<3; R++){

    double di = 0.0;
    double dj = 0.0;

    for(int i=0; i<(int)hit[R].size() - 1; i++){
      di += hit[R][i+1] - hit[R][i];
    }
    di = di/(hit[R].size()-1);

    for(int j=0; j<(int)clust[R].size() - 1; j++){
      dj += clust[R][j+1] - clust[R][j];
    }
    dj = dj/(clust[R].size()-1);
    
    double sum = 0.0;
    int nMatch = 0;
    for(int i=0; i<(int)hit[R].size(); i++){
      for(int j=0; j<(int)clust[R].size(); j++){
	if(fabs(clust[R][j] - hit[R][i]) > (di+dj)/4.0) continue;
	if(j!=0 && clust[R][j] - clust[R][j] > 1.5*M_PI/9.0) continue;
	sum += clust[R][j] - hit[R][i];
	nMatch++;
      }
    }

    avgAngle.push_back(sum/nMatch);

  }
  
  return avgAngle;

}


std::vector<double> PHTpcCentralMembraneMatcher::getRPeaks(TH2F *r_phi){

  TH1D *proj = r_phi->ProjectionY("R_proj",1,360);

  std::vector<double> rPeaks;

  for(int i=2; i<proj->GetNbinsX(); i++){
    if(proj->GetBinContent(i) > 0.15*proj->GetMaximum() && proj->GetBinContent(i) >= proj->GetBinContent(i-1) && proj->GetBinContent(i) >= proj->GetBinContent(i+1)) rPeaks.push_back(proj->GetBinCenter(i));
  }

  for(int i=0; i<(int)rPeaks.size()-1; i++){
    if(rPeaks[i+1]-rPeaks[i] > 0.75) continue;
    if(proj->GetBinContent(proj->FindBin(rPeaks[i])) > proj->GetBinContent(proj->FindBin(rPeaks[i+1]))) rPeaks.erase(std::next(rPeaks.begin(), i+1));
    else rPeaks.erase(std::next(rPeaks.begin(), i));
    i--;
  }
  return rPeaks;
}

int PHTpcCentralMembraneMatcher::getClusterRMatch( std::vector<int> hitMatches, std::vector<double> clusterPeaks, double clusterR){

  int closest_clusterR = -1;

  for(int i=0; i<(int)hitMatches.size(); i++){

    double lowGap = 0.0;
    double highGap = 0.0;

    if(hitMatches[i] <= 14){
      lowGap = 0.565985;
      highGap = 0.565985;
    }else if(hitMatches[i] == 15){
      lowGap = 0.565985;
      highGap = 1.2409686;
    }else if(hitMatches[i] == 16){
      lowGap = 1.2409686;
      highGap = 1.020695;
    }else if(hitMatches[i] >= 17 && hitMatches[i] <= 22){
      lowGap = 1.020695;
      highGap = 1.020695;
    }else if(hitMatches[i] == 23){
      lowGap = 1.020695;
      highGap = 1.5001502;
    }else if(hitMatches[i] == 24){
      lowGap = 1.5001502;
      highGap = 1.09705;
    }else if(hitMatches[i] >= 25){
      lowGap = 1.09705;
      highGap = 1.09705;
    }

    if( clusterR > (clusterPeaks[i] - lowGap) && clusterR <= (clusterPeaks[i] + highGap) ){
      closest_clusterR = hitMatches[i];
      break;
    }
  }

  return closest_clusterR;

}

//____________________________________________________________________________..
int PHTpcCentralMembraneMatcher::InitRun(PHCompositeNode *topNode)
{
  if( m_savehistograms )
  { 

    static constexpr float max_dr = 5.0;
    static constexpr float max_dphi = 0.05;

    fout.reset( new TFile(m_histogramfilename.c_str(),"RECREATE") ); 
    hxy_reco = new TH2F("hxy_reco","reco cluster x:y",800,-100,100,800,-80,80);
    hxy_truth = new TH2F("hxy_truth","truth cluster x:y",800,-100,100,800,-80,80);
    
    hdrdphi = new TH2F("hdrdphi","dr vs dphi",800,-max_dr,max_dr,800,-max_dphi,max_dphi);
    hdrdphi->GetXaxis()->SetTitle("dr");  
    hdrdphi->GetYaxis()->SetTitle("dphi");  
    
    hrdr = new TH2F("hrdr","dr vs r",800,0.0,80.0,800,-max_dr, max_dr);
    hrdr->GetXaxis()->SetTitle("r");  
    hrdr->GetYaxis()->SetTitle("dr");  
    
    hrdphi = new TH2F("hrdphi","dphi vs r",800,0.0,80.0,800,-max_dphi,max_dphi);
    hrdphi->GetXaxis()->SetTitle("r");  
    hrdphi->GetYaxis()->SetTitle("dphi");
    
    hdphi = new TH1F("hdphi","dph",800,-max_dphi,max_dphi);
    hdphi->GetXaxis()->SetTitle("dphi");

    hdr1_single = new TH1F("hdr1_single", "innner dr single", 200,-max_dr, max_dr);
    hdr2_single = new TH1F("hdr2_single", "mid dr single", 200,-max_dr, max_dr);
    hdr3_single = new TH1F("hdr3_single", "outer dr single", 200,-max_dr, max_dr);
    hdr1_double = new TH1F("hdr1_double", "innner dr double", 200,-max_dr, max_dr);
    hdr2_double = new TH1F("hdr2_double", "mid dr double", 200,-max_dr, max_dr);
    hdr3_double = new TH1F("hdr3_double", "outer dr double", 200,-max_dr, max_dr);
    hdrphi = new TH1F("hdrphi","r * dphi", 200, -0.05, 0.05);
    hnclus = new TH1F("hnclus", " nclusters ", 3, 0., 3.);
  }

  fout2.reset( new TFile(m_outputfile2.c_str(),"RECREATE") ); 
    
  hit_r_phi = new TH2F("hit_r_phi","hit r vs #phi;#phi (rad); r (cm)",360,-M_PI,M_PI,500,0,100);
  hit_r_phi_pos = new TH2F("hit_r_phi_pos","hit R vs #phi Z>0;#phi (rad); r (cm)",360,-M_PI,M_PI,500,0,100);
  hit_r_phi_neg = new TH2F("hit_r_phi_neg","hit R vs #phi Z<0;#phi (rad); r (cm)",360,-M_PI,M_PI,500,0,100);

  clust_r_phi = new TH2F("clust_r_phi","clust R vs #phi;#phi (rad); r (cm)",360,-M_PI,M_PI,500,0,100);
  clust_r_phi_pos = new TH2F("clust_r_phi_pos","clust R vs #phi Z>0;#phi (rad); r (cm)",360,-M_PI,M_PI,500,0,100);
  clust_r_phi_neg = new TH2F("clust_r_phi_neg","clust R vs #phi Z<0;#phi (rad); r (cm)",360,-M_PI,M_PI,500,0,100);

  std::vector<double> hitR;
  std::vector<double> hitPhi;

  // Get truth cluster positions
  //=====================
  
  const double phi_petal = M_PI / 9.0;  // angle span of one petal
   

  /*
   * utility function to
   * - duplicate generated truth position to cover both sides of the central membrane
   * - assign proper z,
   * - insert in container
   */
  auto save_truth_position = [&](TVector3 source) 
  {
    source.SetZ( +1 );
    m_truth_pos.push_back( source );

    hit_r_phi->Fill(source.Phi(), source.Perp());
    hit_r_phi_pos->Fill(source.Phi(), source.Perp());

    hitR.push_back(source.Perp());
    hitPhi.push_back(source.Phi());
    
    source.SetZ( -1 );
    m_truth_pos.push_back( source );

    hit_r_phi->Fill(source.Phi(), source.Perp());
    hit_r_phi_neg->Fill(source.Phi(), source.Perp());
  };
  
  // inner region extended is the 8 layers inside 30 cm    
  for(int j = 0; j < nRadii; ++j)
    for(int i=0; i < nGoodStripes_R1_e[j]; ++i)
      for(int k =0; k<18; ++k)
	{
	  TVector3 dummyPos(cx1_e[i][j], cy1_e[i][j], 0.0);
	  dummyPos.RotateZ(k * phi_petal);
	  save_truth_position(dummyPos);

	  if(Verbosity() > 2)	  
	    std::cout << " i " << i << " j " << j << " k " << k << " x1 " << dummyPos.X() << " y1 " << dummyPos.Y()
		      <<  " theta " << std::atan2(dummyPos.Y(), dummyPos.X())
		      << " radius " << get_r( dummyPos.X(), dummyPos.y()) << std::endl; 
	  if(m_savehistograms) hxy_truth->Fill(dummyPos.X(),dummyPos.Y());      	  
	}  
  
  // inner region is the 8 layers outside 30 cm  
  for(int j = 0; j < nRadii; ++j)
    for(int i=0; i < nGoodStripes_R1[j]; ++i)
      for(int k =0; k<18; ++k)
	{
	  TVector3 dummyPos(cx1[i][j], cy1[i][j], 0.0);
	  dummyPos.RotateZ(k * phi_petal);
	  save_truth_position(dummyPos);

	  if(Verbosity() > 2)	  
	    std::cout << " i " << i << " j " << j << " k " << k << " x1 " << dummyPos.X() << " y1 " << dummyPos.Y()
		      <<  " theta " << std::atan2(dummyPos.Y(), dummyPos.X())
		      << " radius " << get_r( dummyPos.X(), dummyPos.y()) << std::endl; 
	  if(m_savehistograms) hxy_truth->Fill(dummyPos.X(),dummyPos.Y());      	  
	}  

  for(int j = 0; j < nRadii; ++j)
    for(int i=0; i < nGoodStripes_R2[j]; ++i)
      for(int k =0; k<18; ++k)
	{
	  TVector3 dummyPos(cx2[i][j], cy2[i][j], 0.0);
	  dummyPos.RotateZ(k * phi_petal);
	  save_truth_position(dummyPos);

	  if(Verbosity() > 2)	  
	    std::cout << " i " << i << " j " << j << " k " << k << " x1 " << dummyPos.X() << " y1 " << dummyPos.Y()
		      <<  " theta " << std::atan2(dummyPos.Y(), dummyPos.X())
		      << " radius " << get_r( dummyPos.X(), dummyPos.y()) << std::endl; 
	  if(m_savehistograms) hxy_truth->Fill(dummyPos.X(),dummyPos.Y());      	  
	}      	  
  
  for(int j = 0; j < nRadii; ++j)
    for(int i=0; i < nGoodStripes_R3[j]; ++i)
      for(int k =0; k<18; ++k)
	{
	  TVector3 dummyPos(cx3[i][j], cy3[i][j], 0.0);
	  dummyPos.RotateZ(k * phi_petal);
	  save_truth_position(dummyPos);

	  if(Verbosity() > 2)
	    std::cout << " i " << i << " j " << j << " k " << k << " x1 " << dummyPos.X() << " y1 " << dummyPos.Y()
		      <<  " theta " << std::atan2(dummyPos.Y(), dummyPos.X())
		      << " radius " << get_r( dummyPos.X(), dummyPos.y()) << std::endl; 
	  if(m_savehistograms) hxy_truth->Fill(dummyPos.X(),dummyPos.Y());      	  
	}

  hit_r_phi_gr = new TGraph((int)hitR.size(), &hitPhi[0], &hitR[0]);

  int ret = GetNodes(topNode);
  return ret;
}

//____________________________________________________________________________..
int PHTpcCentralMembraneMatcher::process_event(PHCompositeNode * /*topNode*/)
{
  std::vector<TVector3> reco_pos;
  std::vector<unsigned int> reco_nclusters;
 
  std::vector<double> clustR;
  std::vector<double> clustPhi;

  std::vector<double> clustR_pos;
  std::vector<double> clustPhi_pos;

  std::vector<double> clustR_neg;
  std::vector<double> clustPhi_neg;


  std::vector<double> clustR1;
  std::vector<double> clustPhi1;

  std::vector<double> clustR_pos1;
  std::vector<double> clustPhi_pos1;

  std::vector<double> clustR_neg1;
  std::vector<double> clustPhi_neg1;

  std::vector<double> clustR2;
  std::vector<double> clustPhi2;

  std::vector<double> clustR_pos2;
  std::vector<double> clustPhi_pos2;

  std::vector<double> clustR_neg2;
  std::vector<double> clustPhi_neg2;



  // reset output distortion correction container histograms
  for( const auto& harray:{m_dcc_out->m_hDRint, m_dcc_out->m_hDPint, m_dcc_out->m_hDZint, m_dcc_out->m_hentries} )
  { for( const auto& h:harray ) { h->Reset(); } }

  clust_r_phi->Reset();
  clust_r_phi_pos->Reset();
  clust_r_phi_neg->Reset();

  // read the reconstructed CM clusters
  auto clusrange = m_corrected_CMcluster_map->getClusters();
  for (auto cmitr = clusrange.first;
       cmitr !=clusrange.second;
       ++cmitr)
    {
      const auto& [cmkey, cmclus_orig] = *cmitr;
      CMFlashClusterv2 *cmclus = dynamic_cast<CMFlashClusterv2 *>(cmclus_orig);
      const unsigned int nclus = cmclus->getNclusters();

      const bool isRGap = cmclus->getIsRGap();

      
      // Do the static + average distortion corrections if the container was found
      Acts::Vector3 pos(cmclus->getX(), cmclus->getY(), cmclus->getZ());
      if( m_dcc_in) pos = m_distortionCorrection.get_corrected_position( pos, m_dcc_in ); 
      
      TVector3 tmp_pos(pos[0], pos[1], pos[2]);

      //      std::cout << "cmkey " << cmkey << "   R: " << tmp_pos.Perp() << "   isgap: " << isRGap << std::endl;

      if(isRGap) continue;

      reco_pos.push_back(tmp_pos);      
      reco_nclusters.push_back(nclus);

      clustR.push_back(tmp_pos.Perp());
      clustPhi.push_back(tmp_pos.Phi());

      if(nclus == 1){
	clustR1.push_back(tmp_pos.Perp());
	clustPhi1.push_back(tmp_pos.Phi());
      }else{
	clustR2.push_back(tmp_pos.Perp());
	clustPhi2.push_back(tmp_pos.Phi());
      }

      clust_r_phi->Fill(tmp_pos.Phi(),tmp_pos.Perp());
      if(tmp_pos.Z() > 0){
	clust_r_phi_pos->Fill(tmp_pos.Phi(),tmp_pos.Perp());
	clustR_pos.push_back(tmp_pos.Perp());
	clustPhi_pos.push_back(tmp_pos.Phi());

	if(nclus == 1){
	  clustR_pos1.push_back(tmp_pos.Perp());
	  clustPhi_pos1.push_back(tmp_pos.Phi());
	}else{
	  clustR_pos2.push_back(tmp_pos.Perp());
	  clustPhi_pos2.push_back(tmp_pos.Phi());
	}

      }else if(tmp_pos.Z() < 0){
	clust_r_phi_neg->Fill(tmp_pos.Phi(),tmp_pos.Perp());
	clustR_neg.push_back(tmp_pos.Perp());
	clustPhi_neg.push_back(tmp_pos.Phi());

	if(nclus == 1){
	  clustR_neg1.push_back(tmp_pos.Perp());
	  clustPhi_neg1.push_back(tmp_pos.Phi());
	}else{
	  clustR_neg2.push_back(tmp_pos.Perp());
	  clustPhi_neg2.push_back(tmp_pos.Phi());
	}
      }

      if(Verbosity())
	{
	  double raw_rad = sqrt( cmclus->getX()*cmclus->getX() + cmclus->getY()*cmclus->getY() );
	  double corr_rad = sqrt( tmp_pos.X()*tmp_pos.X() + tmp_pos.Y()*tmp_pos.Y() );
	  std::cout << "found raw cluster " << cmkey << " with x " << cmclus->getX() << " y " << cmclus->getY() << " z " << cmclus->getZ()   << " radius " << raw_rad << std::endl; 
	  std::cout << "                --- corrected positions: " << tmp_pos.X() << "  " << tmp_pos.Y() << "  " << tmp_pos.Z() << " radius " << corr_rad << std::endl; 
	}

      if(m_savehistograms)
	{      
	  hxy_reco->Fill(tmp_pos.X(), tmp_pos.Y());
	}
    }

  
  clust_r_phi_gr = new TGraph((int)clustR.size(), &clustPhi[0], &clustR[0]);
  clust_r_phi_gr_pos = new TGraph((int)clustR_pos.size(), &clustPhi_pos[0], &clustR_pos[0]);
  clust_r_phi_gr_neg = new TGraph((int)clustR_neg.size(), &clustPhi_neg[0], &clustR_neg[0]);


  clust_r_phi_gr1 = new TGraph((int)clustR1.size(), &clustPhi1[0], &clustR1[0]);
  clust_r_phi_gr1_pos = new TGraph((int)clustR_pos1.size(), &clustPhi_pos1[0], &clustR_pos1[0]);
  clust_r_phi_gr1_neg = new TGraph((int)clustR_neg1.size(), &clustPhi_neg1[0], &clustR_neg1[0]);


  clust_r_phi_gr2 = new TGraph((int)clustR2.size(), &clustPhi2[0], &clustR2[0]);
  clust_r_phi_gr2_pos = new TGraph((int)clustR_pos2.size(), &clustPhi_pos2[0], &clustR_pos2[0]);
  clust_r_phi_gr2_neg = new TGraph((int)clustR_neg2.size(), &clustPhi_neg2[0], &clustR_neg2[0]);

  std::vector<std::vector<double>> hit_phiGaps = getPhiGaps(hit_r_phi);
  std::vector<std::vector<double>> clust_phiGaps = getPhiGaps(clust_r_phi);
  std::vector<double> angleDiff = getAverageRotation(hit_phiGaps, clust_phiGaps);
  

  std::vector<std::vector<double>> hit_phiGaps_pos = getPhiGaps(hit_r_phi_pos);
  std::vector<std::vector<double>> clust_phiGaps_pos = getPhiGaps(clust_r_phi_pos);
  std::vector<double> angleDiff_pos = getAverageRotation(hit_phiGaps_pos, clust_phiGaps_pos);

  std::vector<std::vector<double>> hit_phiGaps_neg = getPhiGaps(hit_r_phi_neg);
  std::vector<std::vector<double>> clust_phiGaps_neg = getPhiGaps(clust_r_phi_neg);
  std::vector<double> angleDiff_neg = getAverageRotation(hit_phiGaps_neg, clust_phiGaps_neg);

  //  std::cout << "rotation R1: " << angleDiff[0] << "   R2: " << angleDiff[1] << "   R3: " << angleDiff[2] << std::endl;
  // std::cout << "pos rotation R1: " << angleDiff_pos[0] << "   R2: " << angleDiff_pos[1] << "   R3: " << angleDiff_pos[2] << std::endl;
  // std::cout << "neg rotation R1: " << angleDiff_neg[0] << "   R2: " << angleDiff_neg[1] << "   R3: " << angleDiff_neg[2] << std::endl;

  double clustRotation[3];
  double clustRotation_pos[3];
  double clustRotation_neg[3];

  clustRotation[0] = getPhiRotation_smoothed(hit_r_phi->ProjectionX("hR1",151,206),clust_r_phi->ProjectionX("cR1",151,206));
  clustRotation[1] = getPhiRotation_smoothed(hit_r_phi->ProjectionX("hR2",206,290),clust_r_phi->ProjectionX("cR2",206,290));
  clustRotation[2] = getPhiRotation_smoothed(hit_r_phi->ProjectionX("hR3",290,499),clust_r_phi->ProjectionX("cR3",290,499));

  clustRotation_pos[0] = getPhiRotation_smoothed(hit_r_phi->ProjectionX("hR1",151,206),clust_r_phi_pos->ProjectionX("cR1_pos",151,206));
  clustRotation_pos[1] = getPhiRotation_smoothed(hit_r_phi->ProjectionX("hR2",206,290),clust_r_phi_pos->ProjectionX("cR2_pos",206,290));
  clustRotation_pos[2] = getPhiRotation_smoothed(hit_r_phi->ProjectionX("hR3",290,499),clust_r_phi_pos->ProjectionX("cR3_pos",290,499));

  clustRotation_neg[0] = getPhiRotation_smoothed(hit_r_phi->ProjectionX("hR1",151,206),clust_r_phi_neg->ProjectionX("cR1_neg",151,206));
  clustRotation_neg[1] = getPhiRotation_smoothed(hit_r_phi->ProjectionX("hR2",206,290),clust_r_phi_neg->ProjectionX("cR2_neg",206,290));
  clustRotation_neg[2] = getPhiRotation_smoothed(hit_r_phi->ProjectionX("hR3",290,499),clust_r_phi_neg->ProjectionX("cR3_neg",290,499));

  std::cout << "clust rotation R1: "<< clustRotation[0] << "   R2: " << clustRotation[1] << "   R3: " << clustRotation[2] << std::endl;
  std::cout << "pos clust rotation R1: "<< clustRotation_pos[0] << "   R2: " << clustRotation_pos[1] << "   R3: " << clustRotation_pos[2] << std::endl;
  std::cout << "neg clust rotation R1: "<< clustRotation_neg[0] << "   R2: " << clustRotation_neg[1] << "   R3: " << clustRotation_neg[2] << std::endl;

  std::vector<double> hit_RPeaks = getRPeaks(hit_r_phi);
  std::vector<double> clust_RPeaks_pos = getRPeaks(clust_r_phi_pos);
  std::vector<double> clust_RPeaks_neg = getRPeaks(clust_r_phi_neg);

  std::vector<double> clust_RGaps_pos;
  int R23Gap_pos = -1;
  for(int i=0; i<(int)clust_RPeaks_pos.size()-1; i++){
    clust_RGaps_pos.push_back(clust_RPeaks_pos[i+1] - clust_RPeaks_pos[i]);
    if(clust_RGaps_pos[i] >= 2.5) R23Gap_pos = i;
  }
  std::cout << "R23Gap_pos: " << R23Gap_pos << std::endl;

  std::vector<double> clust_RGaps_neg;
  int R23Gap_neg = -1;
  for(int i=0; i<(int)clust_RPeaks_neg.size()-1; i++){
    clust_RGaps_neg.push_back(clust_RPeaks_neg[i+1] - clust_RPeaks_neg[i]);
    if(clust_RGaps_neg[i] >= 2.5) R23Gap_neg = i;
  }
  std::cout << "R23Gap_neg: " << R23Gap_neg << std::endl;

  std::cout << "hit matches pos = {";
  std::vector<int> hitMatches_pos;
  for(int i=0; i<(int)clust_RPeaks_pos.size(); i++){
    hitMatches_pos.push_back(i + 23 - R23Gap_pos);
    if(i<(int)clust_RPeaks_pos.size()-1) std::cout << " " << i + 23 - R23Gap_pos << ",";
    else std::cout << " " << i + 23 - R23Gap_pos << "}" << std::endl;
  }

  std::cout << "hit matches neg = {";
  std::vector<int> hitMatches_neg;
  for(int i=0; i<(int)clust_RPeaks_neg.size(); i++){
    hitMatches_neg.push_back(i + 23 - R23Gap_neg);
    if(i<(int)clust_RPeaks_neg.size()-1) std::cout << " " << i + 23 - R23Gap_neg << ",";
    else std::cout << " " << i + 23 - R23Gap_neg << "}" << std::endl;
  }

  
  // Match reco and truth positions
  //std::map<unsigned int, unsigned int> matched_pair;
  std::vector<std::pair<unsigned int, unsigned int>> matched_pair;
  std::vector<unsigned int> matched_nclus;

  std::vector<bool> hits_matched(m_truth_pos.size());
  std::vector<bool> clusts_matched(reco_pos.size());

  for(int matchIt=0; matchIt<2; matchIt++){
  // loop over truth positions
  for(unsigned int i=0; i<m_truth_pos.size(); ++i)
  {

    if(hits_matched[i]) continue;

    const double z1 = m_truth_pos[i].Z(); 
    const double rad1= get_r( m_truth_pos[i].X(),m_truth_pos[i].Y());
    const double phi1 = m_truth_pos[i].Phi();

    int hitRadIndex = -1;
    
    for(int k=0; k<(int)hit_RPeaks.size(); k++){
      if(std::abs(rad1 - hit_RPeaks[k]) < 0.5){
	hitRadIndex = k;
	break;
      }
    }

    //    std::cout << "hit " << i << "   rad: " << rad1 << "   hitRadIndex: " << hitRadIndex << "   hitRPeaks: " << ((hitRadIndex != -1) ? hit_RPeaks[hitRadIndex] : -1) << std::endl;
    
    double prev_dphi = 1.1*m_phi_cut;
    int matchJ = -1;

    // loop over cluster positions
    for(unsigned int j = 0; j < reco_pos.size(); ++j)
    {
      if(clusts_matched[j]) continue;
      
      int angleR = -1;
      
      if(reco_pos[j].Perp() < 41) angleR = 0;
      else if(reco_pos[j].Perp() >= 41 && reco_pos[j].Perp() < 58) angleR = 1;
      if(reco_pos[j].Perp() >= 58) angleR = 2;
      
      
      //const auto& nclus = reco_nclusters[j];
      double phi2 = reco_pos[j].Phi();
      const double z2 = reco_pos[j].Z(); 
      const double rad2=get_r(reco_pos[j].X(), reco_pos[j].Y());
      if(angleR != -1){
	if(z2 > 0) phi2 += (hitRotation[angleR+1] - clustRotation_pos[angleR]);
	else phi2 += (hitRotation[angleR+1] - clustRotation_neg[angleR]);
      }
      

      int clustRMatchIndex = -1;
      if(z2 > 0) clustRMatchIndex = getClusterRMatch(hitMatches_pos, clust_RPeaks_pos, rad2);
      else clustRMatchIndex = getClusterRMatch(hitMatches_neg, clust_RPeaks_neg, rad2);


      if(clustRMatchIndex == -1) continue;

      //const double phi2 = reco_pos[j].Phi();
    
      // only match pairs that are on the same side of the TPC
      const bool accepted_z = ((z1>0)==(z2>0));
      if( !accepted_z ) continue;
 

     
      const bool accepted_r = (hitRadIndex == clustRMatchIndex);

      //      const auto dphi = delta_phi(phi1-phi2);
      const auto dphi = delta_phi(phi1-phi2);
      const bool accepted_phi = std::abs(dphi) < m_phi_cut;
         
      if(!accepted_r || !accepted_phi) continue;
      
      //      std::cout << "matching cluster " << j << "   rad: " << rad2 << "   clustRMatchIndex: " << clustRMatchIndex  << "   hitRadIndex: " << hitRadIndex << "   hitMatchedR: " << (clustRMatchIndex != -1 ? hit_RPeaks[clustRMatchIndex] : -1) << std::endl;


      if(fabs(dphi)<fabs(prev_dphi)){
	prev_dphi = dphi;
	matchJ = j;
	hits_matched[i] = true;
      }
    }//end loop over reco_pos
	
    if(matchJ != -1){
      clusts_matched[matchJ] = true;
      matched_pair.emplace_back(i,matchJ);
      matched_nclus.push_back(reco_nclusters[matchJ]);

      if(m_savehistograms)
	{

	  const auto& nclus = reco_nclusters[matchJ];
	  const double rad2=get_r(reco_pos[matchJ].X(), reco_pos[matchJ].Y());
	  const double phi2 = reco_pos[matchJ].Phi();
	  
	  const auto dr = rad1-rad2;
	  const auto dphi = delta_phi(phi1-phi2);
	  
	  hnclus->Fill( (float) nclus);
	  
	  double r =  rad2;
	  
	  //if( accepted_r )
	  //	    {
	  hdrphi->Fill(r * dphi);
	  hdphi->Fill(dphi);
	  hrdphi->Fill(r,dphi);
	  //	    }
	  
	  //	  if( accepted_r && accepted_phi)
	  hdrdphi->Fill(dr, dphi); 
	  
	  //	     if( accepted_phi )
	  //	    {
	  hrdr->Fill(r,dr);
	  if(nclus==1)
	    {
	      if(r < 40.0) hdr1_single->Fill(dr); 
	      if(r >= 40.0 && r < 58.0) hdr2_single->Fill(dr); 
	      if(r >= 58.0) hdr3_single->Fill(dr); 	  
	    }
	  else
	    {
	      if(r < 40.0) hdr1_double->Fill(dr); 
	      if(r >= 40.0 && r < 58.0) hdr2_double->Fill(dr); 
	      if(r >= 58.0) hdr3_double->Fill(dr); 	  
	    }
	  //	    }
	}//end save histos
    }//end if match was found      
  }//end loop over truth pos      
  }//end loop over matching iterations
  
  // print some statistics: 
  if( Verbosity() )
  {
    const auto n_valid_truth = std::count_if( m_truth_pos.begin(), m_truth_pos.end(), []( const TVector3& pos ) { return get_r( pos.x(), pos.y() ) >  30; } );
    const auto n_reco_size1 = std::count_if( reco_nclusters.begin(), reco_nclusters.end(), []( const unsigned int& value ) { return value==1; } );
    const auto n_reco_size2 = std::count_if( reco_nclusters.begin(), reco_nclusters.end(), []( const unsigned int& value ) { return value==2; } );
    std::cout << "PHTpcCentralMembraneMatcher::process_event - m_truth_pos size: " << m_truth_pos.size() << std::endl;
    std::cout << "PHTpcCentralMembraneMatcher::process_event - m_truth_pos size, r>30cm: " << n_valid_truth << std::endl;
    std::cout << "PHTpcCentralMembraneMatcher::process_event - reco_pos size: " << reco_pos.size() << std::endl;
    std::cout << "PHTpcCentralMembraneMatcher::process_event - reco_pos size (nclus==1): " << n_reco_size1 << std::endl;
    std::cout << "PHTpcCentralMembraneMatcher::process_event - reco_pos size (nclus==2): " << n_reco_size2 << std::endl;
    std::cout << "PHTpcCentralMembraneMatcher::process_event - matched_pair size: " << matched_pair.size() << std::endl;
  }
  
  for(unsigned int ip = 0; ip < matched_pair.size(); ++ip)
  {
    const std::pair<unsigned int, unsigned int>& p = matched_pair[ip];
    const unsigned int& nclus = matched_nclus[ip];

    // add to node tree
    unsigned int key = p.first;
    auto cmdiff = new CMFlashDifferencev1();
    cmdiff->setTruthPhi(m_truth_pos[p.first].Phi());
    cmdiff->setTruthR(m_truth_pos[p.first].Perp());
    cmdiff->setTruthZ(m_truth_pos[p.first].Z());
    
    cmdiff->setRecoPhi(reco_pos[p.second].Phi());
    cmdiff->setRecoR(reco_pos[p.second].Perp());
    cmdiff->setRecoZ(reco_pos[p.second].Z());
    
    cmdiff->setNclusters(nclus);
    
    m_cm_flash_diffs->addDifferenceSpecifyKey(key, cmdiff);
    
    // store cluster position
    const double clus_r = reco_pos[p.second].Perp();
    double clus_phi = reco_pos[p.second].Phi();
    if ( clus_phi < 0 ) clus_phi += 2*M_PI;

    const double clus_z = reco_pos[p.second].z();
    const unsigned int side = (clus_z<0) ? 0:1;
    
    // calculate residuals (cluster - truth)
    const double dr = reco_pos[p.second].Perp() - m_truth_pos[p.first].Perp();
    const double dphi = delta_phi( reco_pos[p.second].Phi() - m_truth_pos[p.first].Phi() );
    const double rdphi = reco_pos[p.second].Perp() * dphi;
    const double dz = reco_pos[p.second].z() - m_truth_pos[p.first].z();

    // fill distortion correction histograms
    /* 
     * TODO: 
     * - we might need to only fill the histograms for cm clusters that have 2 clusters only
     * - we might need a smoothing procedure to fill the bins that have no entries using neighbors
     */
    for( const auto& dcc:{m_dcc_out, m_dcc_out_aggregated.get()} )
    {
      static_cast<TH2*>(dcc->m_hDRint[side])->Fill( clus_phi, clus_r, dr );
      static_cast<TH2*>(dcc->m_hDPint[side])->Fill( clus_phi, clus_r, rdphi );
      static_cast<TH2*>(dcc->m_hDZint[side])->Fill( clus_phi, clus_r, dz );
      static_cast<TH2*>(dcc->m_hentries[side])->Fill( clus_phi, clus_r );
    }
    
  }
  
  if(Verbosity())
  {
    std::cout << "PHTpcCentralMembraneMatcher::process_events - cmclusters: " << m_corrected_CMcluster_map->size() << std::endl;
    std::cout << "PHTpcCentralMembraneMatcher::process_events - matched pairs: " << matched_pair.size() << std::endl;
    std::cout << "PHTpcCentralMembraneMatcher::process_events - differences: " << m_cm_flash_diffs->size() << std::endl;
    std::cout << "PHTpcCentralMembraneMatcher::process_events - entries: " << m_dcc_out->m_hentries[0]->GetEntries() << ", " << m_dcc_out->m_hentries[1]->GetEntries() << std::endl;  
  }

  // normalize per-event distortion correction histograms and fill guarding bins
  normalize_distortions( m_dcc_out );
  fill_guarding_bins( m_dcc_out );
    
  if(Verbosity())
    {	
      // read back differences from node tree as a check
      auto diffrange = m_cm_flash_diffs->getDifferences();
      for (auto cmitr = diffrange.first;
	   cmitr !=diffrange.second;
	   ++cmitr)
	{
	  auto key = cmitr->first;
	  auto cmreco = cmitr->second;
	  
	  std::cout << " key " << key 
		    << " nclus " << cmreco->getNclusters() 
		    << " truth Phi " << cmreco->getTruthPhi() << " reco Phi " << cmreco->getRecoPhi()
		    << " truth R " << cmreco->getTruthR() << " reco R " << cmreco->getRecoR() 
		    << " truth Z " << cmreco->getTruthZ() << " reco Z " << cmreco->getRecoZ() 
		    << std::endl;
	}
    }
  
  return Fun4AllReturnCodes::EVENT_OK;
}

//____________________________________________________________________________..
int PHTpcCentralMembraneMatcher::End(PHCompositeNode * /*topNode*/ )
{

  // write distortion corrections
  if( m_dcc_out_aggregated )
  {
 
    // normalize aggregated distortion correction histograms and fill guarding bins
    normalize_distortions( m_dcc_out_aggregated.get() );
    fill_guarding_bins( m_dcc_out_aggregated.get() );

    // create TFile and write all histograms
    std::unique_ptr<TFile> outputfile( TFile::Open( m_outputfile.c_str(), "RECREATE" ) );
    outputfile->cd();

    // loop over side
    for( unsigned int i = 0; i<2; ++i )
    {
      for( const auto& h:{m_dcc_out_aggregated->m_hDRint[i], m_dcc_out_aggregated->m_hDPint[i], m_dcc_out_aggregated->m_hDZint[i], m_dcc_out_aggregated->m_hentries[i]} )
      { if( h ) h->Write(); }
    }
  }
  
  fout2->cd();

  hit_r_phi->Write();
  hit_r_phi_pos->Write();
  hit_r_phi_neg->Write();

  hit_r_phi_gr->Write("hit_r_phi_gr");
  

  clust_r_phi->Write();
  clust_r_phi_pos->Write();
  clust_r_phi_neg->Write();

  clust_r_phi_gr->Write("clust_r_phi_gr");
  clust_r_phi_gr_pos->Write("clust_r_phi_gr_pos");
  clust_r_phi_gr_neg->Write("clust_r_phi_gr_neg");


  clust_r_phi_gr1->Write("clust_r_phi_gr1");
  clust_r_phi_gr1_pos->Write("clust_r_phi_gr1_pos");
  clust_r_phi_gr1_neg->Write("clust_r_phi_gr1_neg");

  clust_r_phi_gr2->Write("clust_r_phi_gr2");
  clust_r_phi_gr2_pos->Write("clust_r_phi_gr2_pos");
  clust_r_phi_gr2_neg->Write("clust_r_phi_gr2_neg");

  fout2->Close();

  // write evaluation histograms
  if(m_savehistograms && fout)
  {
    fout->cd();
    
    hxy_reco->Write();
    hxy_truth->Write();
    hdrdphi->Write();
    hrdr->Write();
    hrdphi->Write();
    hdphi->Write();
    hdrphi->Write();
    hdr1_single->Write();
    hdr2_single->Write();
    hdr3_single->Write();
    hdr1_double->Write();
    hdr2_double->Write();
    hdr3_double->Write();
    hnclus->Write();
    
    fout->Close();
  }

  return Fun4AllReturnCodes::EVENT_OK;
}

//____________________________________________________________________________..

int  PHTpcCentralMembraneMatcher::GetNodes(PHCompositeNode* topNode)
{
  //---------------------------------
  // Get Objects off of the Node Tree
  //---------------------------------

  m_corrected_CMcluster_map  = findNode::getClass<CMFlashClusterContainer>(topNode, "CORRECTED_CM_CLUSTER");
  if(!m_corrected_CMcluster_map)
    {
      std::cout << PHWHERE << "CORRECTED_CM_CLUSTER Node missing, abort." << std::endl;
      return Fun4AllReturnCodes::ABORTRUN;
    }      

  // input tpc distortion correction
  m_dcc_in = findNode::getClass<TpcDistortionCorrectionContainer>(topNode,"TpcDistortionCorrectionContainer");
  if( m_dcc_in )
    { 
      std::cout << "PHTpcCentralMembraneMatcher:   found TPC distortion correction container" << std::endl; 
    }

  // create node for results of matching
  std::cout << "Creating node CM_FLASH_DIFFERENCES" << std::endl;  
  PHNodeIterator iter(topNode);
  
  // Looking for the DST node
  PHCompositeNode *dstNode = dynamic_cast<PHCompositeNode *>(iter.findFirst("PHCompositeNode", "DST"));
  if (!dstNode)
    {
      std::cout << PHWHERE << "DST Node missing, doing nothing." << std::endl;
      return Fun4AllReturnCodes::ABORTRUN;
    }      
  PHNodeIterator dstiter(dstNode);
  PHCompositeNode *DetNode =
    dynamic_cast<PHCompositeNode *>(dstiter.findFirst("PHCompositeNode", "TRKR"));
  if (!DetNode)
    {
      DetNode = new PHCompositeNode("TRKR");
      dstNode->addNode(DetNode);
    }
  
  m_cm_flash_diffs = new CMFlashDifferenceContainerv1;
  PHIODataNode<PHObject> *CMFlashDifferenceNode =
    new PHIODataNode<PHObject>(m_cm_flash_diffs, "CM_FLASH_DIFFERENCES", "PHObject");
  DetNode->addNode(CMFlashDifferenceNode);
  
//   // output tpc fluctuation distortion container
//   // this one is filled on the fly on a per-CM-event basis, and applied in the tracking chain
//   const std::string dcc_out_node_name = "TpcDistortionCorrectionContainerFluctuation";
//   m_dcc_out = findNode::getClass<TpcDistortionCorrectionContainer>(topNode,dcc_out_node_name);
//   if( !m_dcc_out )
//   { 
//   
//     /// Get the DST node and check
//     auto dstNode = dynamic_cast<PHCompositeNode *>(iter.findFirst("PHCompositeNode", "DST"));
//     if (!dstNode)
//     {
//       std::cout << "PHTpcCentralMembraneMatcher::InitRun - DST Node missing, quitting" << std::endl;
//       return Fun4AllReturnCodes::ABORTRUN;
//     }
//     
//     // Get the tracking subnode and create if not found
//     auto svtxNode = dynamic_cast<PHCompositeNode *>(iter.findFirst("PHCompositeNode", "SVTX"));
//     if (!svtxNode)
//     {
//       svtxNode = new PHCompositeNode("SVTX");
//       dstNode->addNode(svtxNode);
//     }
// 
//     std::cout << "PHTpcCentralMembraneMatcher::GetNodes - creating TpcDistortionCorrectionContainer in node " << dcc_out_node_name << std::endl;
//     m_dcc_out = new TpcDistortionCorrectionContainer;
//     auto node = new PHDataNode<TpcDistortionCorrectionContainer>(m_dcc_out, dcc_out_node_name);
//     svtxNode->addNode(node);
//   }

  // create per event distortions. Do not put on the node tree
  m_dcc_out = new TpcDistortionCorrectionContainer;

  // also prepare the local distortion container, used to aggregate multple events 
  m_dcc_out_aggregated.reset( new TpcDistortionCorrectionContainer );

  // compute axis limits to include guarding bins, needed for TH2::Interpolate to work
  const float phiMin = m_phiMin - (m_phiMax-m_phiMin)/m_phibins;
  const float phiMax = m_phiMax + (m_phiMax-m_phiMin)/m_phibins;
  
  const float rMin = m_rMin - (m_rMax-m_rMin)/m_rbins;
  const float rMax = m_rMax + (m_rMax-m_rMin)/m_rbins;

  // reset all output distortion container so that they match the requested grid size
  const std::array<const std::string,2> extension = {{ "_negz", "_posz" }};
  for( const auto& dcc:{m_dcc_out, m_dcc_out_aggregated.get()} )
  {
    // set dimensions to 2, since central membrane flashes only provide distortions at z = 0
    dcc->dimensions = 2;
    
    // create all histograms
    for( int i =0; i < 2; ++i )
    {
      delete dcc->m_hDPint[i]; dcc->m_hDPint[i] = new TH2F( Form("hIntDistortionP%s", extension[i].c_str()), Form("hIntDistortionP%s", extension[i].c_str()), m_phibins+2, phiMin, phiMax, m_rbins+2, rMin, rMax );
      delete dcc->m_hDRint[i]; dcc->m_hDRint[i] = new TH2F( Form("hIntDistortionR%s", extension[i].c_str()), Form("hIntDistortionR%s", extension[i].c_str()), m_phibins+2, phiMin, phiMax, m_rbins+2, rMin, rMax );
      delete dcc->m_hDZint[i]; dcc->m_hDZint[i] = new TH2F( Form("hIntDistortionZ%s", extension[i].c_str()), Form("hIntDistortionZ%s", extension[i].c_str()), m_phibins+2, phiMin, phiMax, m_rbins+2, rMin, rMax );
      delete dcc->m_hentries[i]; dcc->m_hentries[i] = new TH2I( Form("hEntries%s", extension[i].c_str()), Form("hEntries%s", extension[i].c_str()), m_phibins+2, phiMin, phiMax, m_rbins+2, rMin, rMax );
    }
  }
  
  return Fun4AllReturnCodes::EVENT_OK;
}

//_____________________________________________________________
void PHTpcCentralMembraneMatcher::CalculateCenters(
    int nPads,
    const std::array<double, nRadii>& R,
    std::array<int, nRadii>& nGoodStripes,
    const std::array<int, nRadii>& keepUntil,
    std::array<int, nRadii>& nStripesIn,
    std::array<int, nRadii>& nStripesBefore,
    double cx[][nRadii], double cy[][nRadii])
{
  const double phi_module = M_PI / 6.0;  // angle span of a module
  const int pr_mult = 3;                 // multiples of intrinsic resolution of pads
  const int dw_mult = 8;                 // multiples of diffusion width
  const double diffwidth = 0.6 * mm;     // diffusion width
  const double adjust = 0.015;           //arbitrary angle to center the pattern in a petal

  double theta = 0.0;

  //center coords

  //calculate spacing first:
  std::array<double, nRadii> spacing;
  for (int i = 0; i < nRadii; i++)
  {
    spacing[i] = 2.0 * ((dw_mult * diffwidth / R[i]) + (pr_mult * phi_module / nPads));
  }

  //center calculation
  for (int j = 0; j < nRadii; j++)
  {
    int i_out = 0;
    for (int i = keepThisAndAfter[j]; i < keepUntil[j]; i++)
    {
      if (j % 2 == 0)
      {
        theta = i * spacing[j] + (spacing[j] / 2) - adjust;
        cx[i_out][j] = R[j] * cos(theta) / cm;
        cy[i_out][j] = R[j] * sin(theta) / cm;
      }
      else
      {
        theta = (i + 1) * spacing[j] - adjust;
        cx[i_out][j] = R[j] * cos(theta) / cm;
        cy[i_out][j] = R[j] * sin(theta) / cm;
      }

      if( Verbosity() > 2 )
        std::cout << " j " << j << " i " << i << " i_out " << i_out << " theta " << theta << " cx " << cx[i_out][j] << " cy " << cy[i_out][j] 
        << " radius " << sqrt(pow(cx[i_out][j],2)+pow(cy[i_out][j],2)) << std::endl; 

      i_out++;

      nStripesBefore_R1_e[0] = 0;

      nStripesIn[j] = keepUntil[j] - keepThisAndAfter[j];
      if (j==0)
      {
	nStripesBefore[j] = 0;
      }
      else
      {
        nStripesBefore[j] = nStripesIn[j - 1] + nStripesBefore[j - 1];
      }
      nStripesBefore_R1_e[0] = 0;
    }
    nGoodStripes[j] = i_out;
  }
}


