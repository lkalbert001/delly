/*
============================================================================
DELLY: Structural variant discovery by integrated PE mapping and SR analysis
============================================================================
Copyright (C) 2012 Tobias Rausch

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
============================================================================
Contact: Tobias Rausch (rausch@embl.de)
============================================================================
*/

#include <iostream>
#include <fstream>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphviz.hpp>
#include <boost/graph/connected_components.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/unordered_map.hpp>
#include <boost/program_options/cmdline.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/math/special_functions/pow.hpp>
#include <boost/icl/split_interval_map.hpp>
#include <boost/tokenizer.hpp>
#include <boost/functional/hash.hpp>
#include <boost/filesystem.hpp>
#include <boost/progress.hpp>
#include <htslib/sam.h>

#ifdef OPENMP
#include <omp.h>
#endif

#ifdef PROFILE
#include "gperftools/profiler.h"
#endif

#include "version.h"
#include "util.h"
#include "bolog.h"
#include "tags.h"
#include "dna_score.h"
#include "align_config.h"
#include "align_gotoh.h"
#include "spanning.h"
#include "coverage.h"
#include "junction.h"
#include "msa.h"
#include "split.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <stdio.h>

using namespace torali;

// Config arguments
struct Config {
  unsigned short minMapQual;
  unsigned short minGenoQual;
  unsigned short madCutoff;
  unsigned int minimumFlankSize;
  unsigned int minimumSplitRead;
  unsigned int flankQuality;
  unsigned int graphPruning;
  float epsilon;
  float percentAbnormal;
  std::string svType;
  boost::filesystem::path outfile;
  boost::filesystem::path vcffile;
  boost::filesystem::path genome;
  boost::filesystem::path exclude;
  boost::filesystem::path peDump;
  std::vector<boost::filesystem::path> files;
};


// Reduced bam alignment record data structure
struct BamAlignRecord {
  int32_t tid;         
  int32_t pos;
  int32_t mtid; 
  int32_t mpos;
  int32_t alen;
  int32_t malen;
  int32_t Median;
  int32_t Mad;
  int32_t maxNormalISize;
  int libOrient;
  uint32_t flag;
  uint8_t MapQuality;
  
  BamAlignRecord(bam1_t* rec, uint8_t pairQuality, uint16_t a, uint16_t ma, int32_t median, int32_t mad, int32_t maxISize, int lO) : tid(rec->core.tid), pos(rec->core.pos), mtid(rec->core.mtid), mpos(rec->core.mpos), alen(a), malen(ma), Median(median), Mad(mad), maxNormalISize(maxISize), libOrient(lO), flag(rec->core.flag), MapQuality(pairQuality) {}
};

// Sort reduced bam alignment records
template<typename TRecord>
struct SortBamRecords : public std::binary_function<TRecord, TRecord, bool>
{
  inline bool operator()(TRecord const& s1, TRecord const& s2) const {
    if (s1.tid==s1.mtid) {
      return ((std::min(s1.pos, s1.mpos) < std::min(s2.pos, s2.mpos)) || 
	      ((std::min(s1.pos, s1.mpos) == std::min(s2.pos, s2.mpos)) && (std::max(s1.pos, s1.mpos) < std::max(s2.pos, s2.mpos))) ||
	      ((std::min(s1.pos, s1.mpos) == std::min(s2.pos, s2.mpos)) && (std::max(s1.pos, s1.mpos) == std::max(s2.pos, s2.mpos)) && (s1.maxNormalISize < s2.maxNormalISize)));
    } else {
      return ((s1.pos < s2.pos) ||
	      ((s1.pos == s2.pos) && (s1.mpos < s2.mpos)) ||
	      ((s1.pos == s2.pos) && (s1.mpos == s2.mpos) && (s1.maxNormalISize < s2.maxNormalISize)));
    }
  }
};

// SplitRead struct
struct SplitReadCoord {
  bool forwardRead;
  unsigned int offset;
  int lastKmer;
  int diag;
  std::vector<char> read;
};


template<typename TSequence>
struct OutputRead {
  bool forwardRead;
  unsigned int readOffset;
  TSequence seq;
};

template<typename TRecord>
struct SortSplitReadRecords : public std::binary_function<TRecord, TRecord, bool>
{
  inline bool operator()(TRecord const& s1, TRecord const& s2) {
    if (s1.offset == s2.offset) {
      if (s1.lastKmer == s2.lastKmer) return (s1.diag < s2.diag);
      else return (s1.lastKmer<s2.lastKmer);
    } else return (s1.offset < s2.offset);
  }
};


// Edge struct
template<typename TWeight, typename TVertex>
struct EdgeRecord {
  TVertex source;
  TVertex target;
  TWeight weight;

  EdgeRecord(TVertex s, TVertex t, TWeight w) : source(s), target(t), weight(w) {}
};

// Sort edge records
template<typename TRecord>
struct SortEdgeRecords : public std::binary_function<TRecord, TRecord, bool>
{
  inline bool operator()(TRecord const& e1, TRecord const& e2) const {
    return ((e1.weight < e2.weight) || ((e1.weight == e2.weight) && (e1.source < e2.source)) || ((e1.weight == e2.weight) && (e1.source == e2.source) && (e1.target < e2.target)));
  }
};

// ExcludeInterval
struct ExcludeInterval {
  int32_t tid;
  int32_t start;
  int32_t end;
  
  ExcludeInterval() {}
  ExcludeInterval(int32_t r, int32_t s, int32_t e) : tid(r), start(s), end(e) {}
};

// Sort exclude intervals
template<typename TRecord>
struct SortExcludeIntervals : public std::binary_function<TRecord, TRecord, bool>
{
  inline bool operator()(TRecord const& i1, TRecord const& i2) const {
    return ((i1.tid < i2.tid) || ((i1.tid == i2.tid) && (i1.start < i2.start)) || ((i1.tid == i2.tid) && (i1.start == i2.start) && (i1.end < i2.end)));
  }
};

// Read count struct
struct ReadCount {
  int leftRC;
  int rc;
  int rightRC;

  ReadCount() {}
  ReadCount(int l, int m, int r) : leftRC(l), rc(m), rightRC(r) {}
};

// Deletions
inline std::string
_addID(SVType<DeletionTag>) {
  return "DEL";
}

// Duplications
inline std::string
_addID(SVType<DuplicationTag>) {
  return "DUP";
}

// Inversions
inline std::string
_addID(SVType<InversionTag>) {
  return "INV";
}

// Translocations
inline std::string
_addID(SVType<TranslocationTag>) {
  return "TRA";
}

// Insertion
inline std::string
_addID(SVType<InsertionTag>) {
  return "INS";
}

// Decode Orientation
inline int
_decodeOrientation(std::string const& value) {
  if (value=="3to3") return 0;
  else if (value=="5to5") return 1;
  else if (value=="3to5") return 2;
  else if (value=="5to3") return 3;
  else return 4;
}

// Add Orientation
inline std::string
_addOrientation(int const ct) {
  if (ct==0) return "3to3";
  else if (ct==1) return "5to5";
  else if (ct==2) return "3to5";
  else if (ct==3) return "5to3";
  else return "NtoN";
}

// Parse Delly vcf file
template<typename TConfig, typename TRefNames, typename TRefLen, typename TSize, typename TStructuralVariantRecord, typename TTag>
inline void
vcfParse(TConfig const& c, TRefNames const& refnames, TRefLen const& reflen, TSize const overallMaxISize, std::vector<TStructuralVariantRecord>& svs, SVType<TTag> svType)
{
  bool refPresent=false;
  if (boost::filesystem::exists(c.genome) && boost::filesystem::is_regular_file(c.genome) && boost::filesystem::file_size(c.genome)) refPresent=true;
  std::ifstream vcfFile(c.vcffile.string().c_str(), std::ifstream::in);
  if (vcfFile.is_open()) {
    typedef boost::unordered_map<std::string, unsigned int> TMapChr;
    TMapChr mapChr;
    for(unsigned int i = 0; i<refnames.size(); ++i) mapChr[ refnames[i] ] = i;
    while (vcfFile.good()) {
      std::string vcfLine;
      getline(vcfFile, vcfLine);
      if (vcfLine[0]=='#') continue;
      typedef boost::tokenizer< boost::char_separator<char> > Tokenizer;
      boost::char_separator<char> sep("\t");
      Tokenizer tokens(vcfLine, sep);
      Tokenizer::iterator tokIter = tokens.begin();
      if (tokIter!=tokens.end()) {
	std::string chr=*tokIter++;
	TMapChr::const_iterator mapChrIt = mapChr.find(chr);
	if (mapChrIt != mapChr.end()) {
	  if (tokIter!=tokens.end()) {
	    StructuralVariantRecord svRec;
	    svRec.chr = mapChrIt->second;
	    svRec.chr2 = mapChrIt->second;
	    svRec.svStart = boost::lexical_cast<int32_t>(*tokIter++);
	    std::string id = *tokIter++;
	    if (id.substr(0,3)!=_addID(svType)) continue;
	    svRec.id = parseSVid(id);
	    svRec.peSupport=0;
	    svRec.peMapQuality=0;
	    svRec.srSupport=0;
	    svRec.srAlignQuality=0;
	    svRec.wiggle = 0;
	    svRec.precise = false;
	    // Ignore ref, alt, qual and filter
	    tokIter++; tokIter++; tokIter++; tokIter++;
	    // Parse info string
	    std::string infoStr = *tokIter++;
	    boost::char_separator<char> sepInfo(";");
	    Tokenizer infoTokens(infoStr, sepInfo);
	    Tokenizer::iterator infoIter = infoTokens.begin();
	    for (;infoIter!=infoTokens.end();++infoIter) {
	      std::string keyValue = *infoIter;
	      std::size_t found = keyValue.find('=');
	      if (found==std::string::npos) {
		if ((keyValue=="PRECISE") && (refPresent)) svRec.precise=true;
		continue;
	      }
	      std::string key = keyValue.substr(0, found);
	      std::string value = keyValue.substr(found+1);
	      if (key == "PE") svRec.peSupport = boost::lexical_cast<int>(value);
	      else if (key == "MAPQ") svRec.peMapQuality = (uint8_t) boost::lexical_cast<uint16_t>(value); // lexical_cast does not work for uint8_t
	      else if (key == "SR") svRec.srSupport = boost::lexical_cast<int>(value);
	      else if (key == "SRQ") svRec.srAlignQuality = boost::lexical_cast<double>(value);
	      else if (key == "CHR2") {
		TMapChr::const_iterator mapChr2It = mapChr.find(value);
		if (mapChr2It != mapChr.end()) svRec.chr2 = mapChr2It->second;
	      }
	      else if (key == "END") svRec.svEnd = boost::lexical_cast<int32_t>(value);
	      else if (key == "CONSENSUS") svRec.consensus = value;
	      else if (key == "CIPOS") {
		std::size_t foundComma = value.find(',');
		int off1 = boost::lexical_cast<int>(value.substr(0, foundComma));
		int off2 = boost::lexical_cast<int>(value.substr(foundComma+1));
		if (abs(off1)>svRec.wiggle) svRec.wiggle=abs(off1);
		if (abs(off2)>svRec.wiggle) svRec.wiggle=abs(off2);
	      }
	      else if (key == "CIEND") {
		std::size_t foundComma = value.find(',');
		int endOff1 = boost::lexical_cast<int>(value.substr(0, foundComma));
		int endOff2 = boost::lexical_cast<int>(value.substr(foundComma+1));
		if (abs(endOff1)>svRec.wiggle) svRec.wiggle=abs(endOff1);
		if (abs(endOff2)>svRec.wiggle) svRec.wiggle=abs(endOff2);
	      }
	      else if (key == "CT") svRec.ct = _decodeOrientation(value);
	      else continue;
	    }
	    svRec.svStartBeg = std::max(svRec.svStart - 1 - overallMaxISize, 0);
	    svRec.svStartEnd = std::min((uint32_t) svRec.svStart - 1 + overallMaxISize, reflen[svRec.chr]);
	    svRec.svEndBeg = std::max(svRec.svEnd - 1 - overallMaxISize, 0);
	    svRec.svEndEnd = std::min((uint32_t) svRec.svEnd - 1 + overallMaxISize, reflen[svRec.chr2]);
	    if ((svRec.chr==svRec.chr2) && (svRec.svStartEnd > svRec.svEndBeg)) {
	      unsigned int midPointDel = ((svRec.svEnd - svRec.svStart) / 2) + svRec.svStart;
	      svRec.svStartEnd = midPointDel -1;
	      svRec.svEndBeg = midPointDel;
	    }
	    svs.push_back(svRec);
	  }
	}
      }
    }
    vcfFile.close();
  }
}


// Insertion length
template<typename TSize, typename TTag>
inline TSize
_addInsertionLength(TSize, SVType<TTag>) {
  return 0;
}

// Insertion length
template<typename TSize>
inline TSize
_addInsertionLength(TSize l, SVType<InsertionTag>) {
  return l;
}


template<typename TConfig, typename TStructuralVariantRecord, typename TJunctionCountMap, typename TReadCountMap, typename TCountMap, typename TTag>
inline void
vcfOutput(TConfig const& c, std::vector<TStructuralVariantRecord> const& svs, TJunctionCountMap const& jctCountMap, TReadCountMap const& readCountMap, TCountMap const& spanCountMap, SVType<TTag> svType) 
{
  // Typedefs
  typedef typename TCountMap::key_type TSampleSVPair;

  // Get the references
  typedef std::vector<std::string> TRefNames;
  TRefNames refnames;
  if (refnames.empty()) {
    samFile* samfile = sam_open(c.files[0].string().c_str(), "r");
    bam_hdr_t* hdr = sam_hdr_read(samfile);
    for (int i = 0; i<hdr->n_targets; ++i) refnames.push_back(hdr->target_name[i]);
    bam_hdr_destroy(hdr);
    sam_close(samfile);
  }

  // Output all structural variants
  std::ofstream ofile(c.outfile.string().c_str());

  // Print vcf header
  boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
  boost::gregorian::date today = now.date();
  ofile << "##fileformat=VCFv4.1" << std::endl;
  ofile << "##fileDate=" << boost::gregorian::to_iso_string(today) << std::endl;
  ofile << "##ALT=<ID=DEL,Description=\"Deletion\">" << std::endl;
  ofile << "##ALT=<ID=DUP,Description=\"Duplication\">" << std::endl;
  ofile << "##ALT=<ID=INV,Description=\"Inversion\">" << std::endl;
  ofile << "##ALT=<ID=TRA,Description=\"Translocation\">" << std::endl;
  ofile << "##ALT=<ID=INS,Description=\"Insertion\">" << std::endl;
  ofile << "##FILTER=<ID=LowQual,Description=\"PE support below 3 or mapping quality below 20.\">" << std::endl;
  ofile << "##INFO=<ID=CIEND,Number=2,Type=Integer,Description=\"PE confidence interval around END\">" << std::endl;
  ofile << "##INFO=<ID=CIPOS,Number=2,Type=Integer,Description=\"PE confidence interval around POS\">" << std::endl;
  ofile << "##INFO=<ID=CHR2,Number=1,Type=String,Description=\"Chromosome for END coordinate in case of a translocation\">" << std::endl;
  ofile << "##INFO=<ID=END,Number=1,Type=Integer,Description=\"End position of the structural variant\">" << std::endl;
  ofile << "##INFO=<ID=PE,Number=1,Type=Integer,Description=\"Paired-end support of the structural variant\">" << std::endl;
  ofile << "##INFO=<ID=MAPQ,Number=1,Type=Integer,Description=\"Median mapping quality of paired-ends\">" << std::endl;
  ofile << "##INFO=<ID=SR,Number=1,Type=Integer,Description=\"Split-read support\">" << std::endl;
  ofile << "##INFO=<ID=SRQ,Number=1,Type=Float,Description=\"Split-read consensus alignment quality\">" << std::endl;
  ofile << "##INFO=<ID=CONSENSUS,Number=1,Type=String,Description=\"Split-read consensus sequence\">" << std::endl;
  ofile << "##INFO=<ID=CT,Number=1,Type=String,Description=\"Paired-end signature induced connection type\">" << std::endl;
  ofile << "##INFO=<ID=IMPRECISE,Number=0,Type=Flag,Description=\"Imprecise structural variation\">" << std::endl;
  ofile << "##INFO=<ID=PRECISE,Number=0,Type=Flag,Description=\"Precise structural variation\">" << std::endl;
  ofile << "##INFO=<ID=SVTYPE,Number=1,Type=String,Description=\"Type of structural variant\">" << std::endl;
  ofile << "##INFO=<ID=SVMETHOD,Number=1,Type=String,Description=\"Type of approach used to detect SV\">" << std::endl;
  ofile << "##INFO=<ID=INSLEN,Number=1,Type=Integer,Description=\"Predicted length of the insertion\">" << std::endl;
  ofile << "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">" << std::endl;
  ofile << "##FORMAT=<ID=GL,Number=G,Type=Float,Description=\"Log10-scaled genotype likelihoods for RR,RA,AA genotypes\">" << std::endl;
  ofile << "##FORMAT=<ID=GQ,Number=1,Type=Integer,Description=\"Genotype Quality\">" << std::endl;
  ofile << "##FORMAT=<ID=FT,Number=1,Type=String,Description=\"Per-sample genotype filter\">" << std::endl;
  ofile << "##FORMAT=<ID=RC,Number=1,Type=Integer,Description=\"Raw high-quality read counts for the SV\">" << std::endl;
  ofile << "##FORMAT=<ID=RCL,Number=1,Type=Integer,Description=\"Raw high-quality read counts for the left control region\">" << std::endl;
  ofile << "##FORMAT=<ID=RCR,Number=1,Type=Integer,Description=\"Raw high-quality read counts for the right control region\">" << std::endl;
  ofile << "##FORMAT=<ID=CN,Number=1,Type=Integer,Description=\"Read-depth based copy-number estimate for autosomal sites\">" << std::endl;
  ofile << "##FORMAT=<ID=DR,Number=1,Type=Integer,Description=\"# high-quality reference pairs\">" << std::endl;
  ofile << "##FORMAT=<ID=DV,Number=1,Type=Integer,Description=\"# high-quality variant pairs\">" << std::endl;
  ofile << "##FORMAT=<ID=RR,Number=1,Type=Integer,Description=\"# high-quality reference junction reads\">" << std::endl;
  ofile << "##FORMAT=<ID=RV,Number=1,Type=Integer,Description=\"# high-quality variant junction reads\">" << std::endl;
  ofile << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT";
  for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
    std::string sampleName(c.files[file_c].stem().string());
    ofile << "\t" << sampleName;
  }
  ofile << std::endl;

  // Iterate all structural variants
  typedef std::vector<TStructuralVariantRecord> TSVs;
  typename TSVs::const_iterator svIter = svs.begin();
  typename TSVs::const_iterator svIterEnd = svs.end();
  now = boost::posix_time::second_clock::local_time();
  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Genotyping" << std::endl;
  boost::progress_display show_progress( svs.size() );
  for(;svIter!=svIterEnd;++svIter) {
    ++show_progress;

    // Output main vcf fields
    std::string filterField="PASS";
    if ((svIter->peSupport < 3) || (svIter->peMapQuality < 20) || ( (svIter->chr != svIter->chr2) && (svIter->peSupport < 5) ) ) {
      filterField="LowQual";
    }
    
    std::stringstream id;
    id << _addID(svType) << std::setw(8) << std::setfill('0') << svIter->id;
    ofile << refnames[svIter->chr] << "\t" << svIter->svStart << "\t" << id.str() << "\tN\t<" << _addID(svType) << ">\t.\t" <<  filterField << "\t";

    // Add info fields
    if (svIter->precise) ofile << "PRECISE;";
    else ofile << "IMPRECISE;";
    ofile << "CIEND=" << -svIter->wiggle << "," << svIter->wiggle << ";CIPOS=" << -svIter->wiggle << "," << svIter->wiggle << ";";
    ofile << "SVTYPE=" << _addID(svType) << ";";
    ofile << "SVMETHOD=EMBL.DELLYv" << dellyVersionNumber << ";";
    ofile << "CHR2=" << refnames[svIter->chr2] << ";";
    ofile << "END=" << svIter->svEnd << ";";
    ofile << "CT=" << _addOrientation(svIter->ct) << ";";
    ofile << "INSLEN=" << _addInsertionLength(svIter->insLen, svType) << ";";
    ofile << "PE=" << svIter->peSupport << ";";
    ofile << "MAPQ=" << (int) svIter->peMapQuality;
    if (svIter->precise)  {
      ofile << ";SR=" << svIter->srSupport;
      ofile << ";SRQ=" << svIter->srAlignQuality;
      ofile << ";CONSENSUS=" << svIter->consensus;
    }

    // Add genotype columns (right bp only across all samples)
    ofile << "\tGT:GL:GQ:FT:RCL:RC:RCR:CN:DR:DV:RR:RV";
    for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
      // Get the sample name
      std::string sampleName(c.files[file_c].stem().string());
      TSampleSVPair sampleSVPairLeft = std::make_pair(sampleName, svIter->id);
      TSampleSVPair sampleSVPairRight = std::make_pair(sampleName, -svIter->id);
      typename TJunctionCountMap::const_iterator jctCountMapIt=jctCountMap.find(sampleSVPairLeft);
      typename TCountMap::const_iterator spanLeftIt=spanCountMap.find(sampleSVPairLeft);
      typename TCountMap::const_iterator spanRightIt=spanCountMap.find(sampleSVPairRight);

      // Counters
      int drCount = 0;
      int dvCount = 0;
      int rrCount = 0;
      int rvCount = 0;
      int leftRC = 0;
      int rcCount = 0;
      int rightRC = 0;

      // Compute GLs
      double gl[3];
      int gqVal;
      std::string gtype;
      bool trGL;
      if (svIter->precise) {
	trGL = _computeGLs(jctCountMapIt->second.first, jctCountMapIt->second.second, &gl[0], gqVal, gtype);
	if (jctCountMapIt!=jctCountMap.end()) {
	  rrCount = jctCountMapIt->second.first.size();
	  rvCount = jctCountMapIt->second.second.size();
	}
      } else {
	double glLeft[3];
	int gqValLeft;
	std::string gtypeLeft;
	bool trGLLeft;
	trGLLeft = _computeGLs(spanLeftIt->second.first, spanLeftIt->second.second, &glLeft[0], gqValLeft, gtypeLeft);
	double glRight[3];
	int gqValRight;
	std::string gtypeRight;
	bool trGLRight;
	trGLRight = _computeGLs(spanRightIt->second.first, spanRightIt->second.second, &glRight[0], gqValRight, gtypeRight);
	//if (gqValLeft > gqValRight) {
	if (spanLeftIt->second.first.size()<spanRightIt->second.first.size()) {
	  trGL = trGLLeft;
	  gl[0] = glLeft[0]; gl[1] = glLeft[1]; gl[2] = glLeft[2];
	  gqVal = gqValLeft;
	  gtype = gtypeLeft;
	  drCount=spanLeftIt->second.first.size();
	  dvCount=spanLeftIt->second.second.size();
	} else {
	  trGL = trGLRight;
	  gl[0] = glRight[0]; gl[1] = glRight[1]; gl[2] = glRight[2];
	  gqVal = gqValRight;
	  gtype = gtypeRight;
	  drCount=spanRightIt->second.first.size();
	  dvCount=spanRightIt->second.second.size();
	}
	//std::cerr << id.str() << "\t" << sampleName << "\tGTLeft:" << gtypeLeft << "\tGLLeft:" << glLeft[2] << "," << glLeft[1] << "," << glLeft[0] << "\tGQLeft:" << gqValLeft << "\tDRLeft:" << spanLeftIt->second.first.size() << "\tDVLeft:" << spanLeftIt->second.second.size() << "\tGTRight:" << gtypeRight << "\tGLRight:" << glRight[2] << "," << glRight[1] << "," << glRight[0] << "\tGQRight:" << gqValRight << "\tDRRight:" << spanRightIt->second.first.size() << "\tDVRight:" << spanRightIt->second.second.size() << std::endl;
      }
      typename TReadCountMap::const_iterator readCountMapIt=readCountMap.find(sampleSVPairLeft);
      if (readCountMapIt!=readCountMap.end()) {
	leftRC = readCountMapIt->second.leftRC;
	rcCount = readCountMapIt->second.rc;
	rightRC = readCountMapIt->second.rightRC;
      }	
      int cnEst = -1;
      if ((leftRC + rightRC) > 0) cnEst = boost::math::iround( 2.0 * (double) rcCount / (double) (leftRC + rightRC) );

      // Output genotypes
      if (trGL) {
	ofile << "\t" << gtype << ":" << gl[2] << "," << gl[1] << "," << gl[0] << ":" << gqVal << ":";
	if (gqVal<15) ofile << "LowQual:";
	else ofile << "PASS:";
      } else {
	ofile << "\t./.:.,.,.:0:LowQual:";
      }
      ofile << leftRC << ":" << rcCount << ":" << rightRC << ":" << cnEst << ":" << drCount << ":" << dvCount << ":" << rrCount << ":" << rvCount;
    }
    ofile << std::endl;
  }

  ofile.close();
}


inline bool _validSoftClip(bam1_t* rec, int& clipSize, int& splitPoint, bool& leadingSC) {
  // Check read-length
  if (rec->core.l_qseq < 35) return false;

  // Check for soft-clips
  bool hasSoftClip = false;
  uint32_t* cigar = bam_get_cigar(rec);
  for (unsigned int i = 0; i < rec->core.n_cigar; ++i) 
    if (bam_cigar_op(cigar[i]) == BAM_CSOFT_CLIP) hasSoftClip = true;
  if (!hasSoftClip) return false;

  // Get quality vector
  typedef std::vector<uint8_t> TQuality;
  TQuality quality;
  quality.resize(rec->core.l_qseq);
  uint8_t* qualptr = bam_get_qual(rec);
  for (int i = 0; i < rec->core.l_qseq; ++i) quality[i] = qualptr[i];

  // Get soft-clips
  unsigned int alen = 0;
  unsigned int numSoftClip = 0;
  unsigned int meanQuality = 0;
  for (unsigned int i = 0; i < rec->core.n_cigar; ++i) {
    if ((bam_cigar_op(cigar[i]) == BAM_CMATCH) || (bam_cigar_op(cigar[i]) == BAM_CINS)) alen += bam_cigar_oplen(cigar[i]);
    else if (bam_cigar_op(cigar[i]) == BAM_CSOFT_CLIP) {
      if (!alen) leadingSC = true;
      else leadingSC = false;
      ++numSoftClip;
      clipSize = bam_cigar_oplen(cigar[i]);
      splitPoint = rec->core.pos + alen;
      unsigned int qualSum = 0;
      for(unsigned int i = alen; i < (alen+clipSize); ++i) qualSum += quality[i];
      meanQuality = qualSum / clipSize;
    }
  }
  //std::cerr << numSoftClip << ',' << clipSize << ',' << meanQuality << ',' << splitPoint << std::endl;
  return ((numSoftClip==1) && (meanQuality>=20));
}

template<typename TBPoint, typename TCT>
inline bool
_validSCOrientation(TBPoint bpPoint, bool leadingSC, TCT, SVType<DeletionTag>) 
{
  if (((!bpPoint) && (!leadingSC)) || ((bpPoint) && (leadingSC))) return true;
  else return false;
}

template<typename TBPoint, typename TCT>
inline bool
_validSCOrientation(TBPoint bpPoint, bool leadingSC, TCT, SVType<DuplicationTag>) 
{
  if (((!bpPoint) && (leadingSC)) || ((bpPoint) && (!leadingSC))) return true;
  else return false;
}

template<typename TBPoint, typename TCT>
inline bool
_validSCOrientation(TBPoint, bool leadingSC, TCT ct, SVType<InversionTag>) 
{
  return (ct ? leadingSC : (!leadingSC));
}

template<typename TBPoint, typename TCT>
inline bool
_validSCOrientation(TBPoint bpPoint, bool leadingSC, TCT ct, SVType<TranslocationTag>) 
{
  if (ct == 0) return (!leadingSC);
  else if (ct == 1) return leadingSC;
  else if (ct == 2) {
    if (((!bpPoint) && (!leadingSC)) || ((bpPoint) && (leadingSC))) return true;
    else return false;
  } 
  else if (ct == 3) {
    if (((!bpPoint) && (leadingSC)) || ((bpPoint) && (!leadingSC))) return true;
    else return false;
  } else return false;
}

template<typename TBPoint, typename TCT>
inline void
_adjustOrientation(std::string&, TBPoint, TCT, SVType<DeletionTag>) 
{
  //Nop
}

template<typename TBPoint, typename TCT>
inline void
_adjustOrientation(std::string&, TBPoint, TCT, SVType<DuplicationTag>) 
{
  //Nop
}

template<typename TBPoint, typename TCT>
inline void
_adjustOrientation(std::string& sequence, TBPoint bpPoint, TCT ct, SVType<InversionTag>) 
{
  if (((!ct) && (bpPoint)) || ((ct) && (!bpPoint))) {
    std::string rev = boost::to_upper_copy(std::string(sequence.rbegin(), sequence.rend()));
    std::size_t i = 0;
    for(std::string::iterator revIt = rev.begin(); revIt != rev.end(); ++revIt, ++i) {
      switch (*revIt) {
      case 'A': sequence[i]='T'; break;
      case 'C': sequence[i]='G'; break;
      case 'G': sequence[i]='C'; break;
      case 'T': sequence[i]='A'; break;
      case 'N': sequence[i]='N'; break;
      default: break;
      }
    }
  }
}

template<typename TBPoint, typename TCT>
inline void
_adjustOrientation(std::string& sequence, TBPoint bpPoint, TCT ct, SVType<TranslocationTag>) 
{
  if (((ct==0) && (bpPoint)) || ((ct==1) && (!bpPoint))) {
    std::string rev = boost::to_upper_copy(std::string(sequence.rbegin(), sequence.rend()));
    std::size_t i = 0;
    for(std::string::iterator revIt = rev.begin(); revIt != rev.end(); ++revIt, ++i) {
      switch (*revIt) {
      case 'A': sequence[i]='T'; break;
      case 'C': sequence[i]='G'; break;
      case 'G': sequence[i]='C'; break;
      case 'T': sequence[i]='A'; break;
      case 'N': sequence[i]='N'; break;
      default: break;
      }
    }
  }
}

template<typename TValue, typename TPosition>
inline void
_movingAverage(std::vector<TValue> const& spp, TPosition const windowSize, TValue& movingAverage, TPosition& lowerBound, TPosition& upperBound) {
  movingAverage = 0;
  for(TPosition i = 0; (i<windowSize) && (i < (TPosition) spp.size()); ++i) movingAverage += spp[i];
  TValue bestAverage = movingAverage;
  TValue bestAverageIndex = windowSize - 1;
  for(std::size_t i = windowSize; i < spp.size() ; ++i) {
    movingAverage -= spp[i-windowSize];
    movingAverage += spp[i];
    if (movingAverage>bestAverage) {
      bestAverage = movingAverage;
      bestAverageIndex = i;
    }
  }
  movingAverage = bestAverage;
  upperBound = bestAverageIndex + 1;
  if (upperBound > windowSize) lowerBound = upperBound - windowSize;
  else lowerBound = 0;
}


template<typename TConfig, typename TStructuralVariantRecord>
inline bool
findPutativeSplitReads(TConfig const&, std::vector<TStructuralVariantRecord>&,  SVType<InsertionTag>) 
{
  return false;
}

template<typename TConfig, typename TStructuralVariantRecord, typename TTag>
inline bool
findPutativeSplitReads(TConfig const& c, std::vector<TStructuralVariantRecord>& svs,  SVType<TTag> svType) 
{
  typedef std::vector<TStructuralVariantRecord> TSVs;

  // Open file handles
  typedef std::vector<std::string> TRefNames;
  TRefNames refnames;
  typedef std::vector<samFile*> TSamFile;
  typedef std::vector<hts_idx_t*> TIndex;
  TSamFile samfile;
  TIndex idx;
  samfile.resize(c.files.size());
  idx.resize(c.files.size());
  for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
    samfile[file_c] = sam_open(c.files[file_c].string().c_str(), "r");
    idx[file_c] = sam_index_load(samfile[file_c], c.files[file_c].string().c_str());
    if (!file_c) {
      bam_hdr_t* hdr = sam_hdr_read(samfile[file_c]);
      for (int i = 0; i<hdr->n_targets; ++i) refnames.push_back(hdr->target_name[i]);
      bam_hdr_destroy(hdr);
    }
  }

  // Parse genome, no single-anchored reads anymore only soft-clipped reads
  unsigned int totalSplitReadsAligned = 0;
  kseq_t *seq;
  int l;
  gzFile fp = gzopen(c.genome.string().c_str(), "r");
  seq = kseq_init(fp);
  boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Split-read alignment" << std::endl;
  boost::progress_display show_progress( refnames.size() );
  while ((l = kseq_read(seq)) >= 0) {
    // Find reference index
    for(int32_t refIndex=0; refIndex < (int32_t) refnames.size(); ++refIndex) {
      if (seq->name.s == refnames[refIndex]) {
	++show_progress;

	// Iterate all structural variants on this chromosome
	typename TSVs::iterator svIt = svs.begin();
	typename TSVs::iterator svItEnd = svs.end();
	for(;svIt!=svItEnd; ++svIt) {
	  if ((svIt->chr != svIt->chr2) && (svIt->chr2 == refIndex)) {
	    // For translocations temporarily store the first reference part in the consensus string
	    svIt->consensus = _getSVRef(seq->seq.s, *svIt, refIndex, svType);
	  }
	  if (svIt->chr == refIndex) {
	    // Get the SV reference
	    std::string svRefStr = _getSVRef(seq->seq.s, *svIt, refIndex, svType);
	    svIt->consensus = "";
	    typedef std::vector<std::pair<int, std::string> > TOffsetSplit;
	    typedef std::vector<int> TSplitPoints;
	    TOffsetSplit osp0;
	    TSplitPoints spp0;
	    TOffsetSplit osp1;
	    TSplitPoints spp1;

	    // Find putative split reads in all samples
	    for (unsigned int bpPoint = 0; bpPoint<2; ++bpPoint) {
	      int32_t regionChr = svIt->chr;
	      int regionStart = (svIt->svStartBeg + svIt->svStart)/2;
	      int regionEnd = (svIt->svStart + svIt->svStartEnd)/2;
	      if (bpPoint) {
		regionChr = svIt->chr2;
		regionStart = (svIt->svEndBeg + svIt->svEnd)/2;
		regionEnd = (svIt->svEnd + svIt->svEndEnd)/2;
		spp1.resize(regionEnd-regionStart, 0);
	      } else {
		spp0.resize(regionEnd-regionStart, 0);
	      }
#pragma omp parallel for default(shared)
	      for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
		hts_itr_t* iter = sam_itr_queryi(idx[file_c], regionChr, regionStart, regionEnd);
		bam1_t* rec = bam_init1();
		while (sam_itr_next(samfile[file_c], iter, rec) >= 0) {
		  if (rec->core.flag & (BAM_FSECONDARY | BAM_FQCFAIL | BAM_FDUP | BAM_FSUPPLEMENTARY | BAM_FUNMAP)) continue;

		  // Valid soft clip?
		  int clipSize = 0;
		  int splitPoint = 0;
		  bool leadingSoftClip = false;
		  if (_validSoftClip(rec, clipSize, splitPoint, leadingSoftClip)) {
		    if ((splitPoint >= regionStart) && (splitPoint < regionEnd)) {
		      splitPoint -= regionStart;
		      // Minimum clip size
		      int minClipSize = (int) (log10(rec->core.l_qseq) * 10);
		      if (clipSize > minClipSize) {
			// Leading or trailing softclip?
			if (_validSCOrientation(bpPoint, leadingSoftClip, svIt->ct, svType)) {
			  // Get the sequence
			  std::string sequence;
			  sequence.resize(rec->core.l_qseq);
			  uint8_t* seqptr = bam_get_seq(rec);
			  for (int i = 0; i < rec->core.l_qseq; ++i) sequence[i] = "=ACMGRSVTWYHKDBN"[bam_seqi(seqptr, i)];
			  
			  // Reverse complement iff necesssary
			  _adjustOrientation(sequence, bpPoint, svIt->ct, svType);
			  
			  if (bpPoint) {
#pragma omp critical
			    {
			      ++spp1[splitPoint];
			      osp1.push_back(std::make_pair(splitPoint, sequence));
			    } 
			  } else {
#pragma omp critical
			    {
			      ++spp0[splitPoint];
			      osp0.push_back(std::make_pair(splitPoint, sequence));
			    } 
			  }
			}
		      }
		    }
		  }
		}
		bam_destroy1(rec);
		hts_itr_destroy(iter);		
	      }
	    }
	    // Collect candidate split reads
	    typedef std::set<std::string> TSplitReadSet;
	    TSplitReadSet splitReadSet;
	    int mvAvg, lBound, uBound;
	    _movingAverage(spp0, 5, mvAvg, lBound, uBound);
	    if (mvAvg > 0) 
	      for(typename TOffsetSplit::const_iterator itOS = osp0.begin(); itOS != osp0.end(); ++itOS) 
		if ((itOS->first >= lBound) && (itOS->first < uBound)) 
		  if (splitReadSet.size() < 1000) splitReadSet.insert(itOS->second); // Limit to at most 1000 split reads
	    _movingAverage(spp1, 5, mvAvg, lBound, uBound);
	    if (mvAvg > 0) {
	      for(typename TOffsetSplit::const_iterator itOS = osp1.begin(); itOS != osp1.end(); ++itOS) 
		if ((itOS->first >= lBound) && (itOS->first < uBound)) 
		  if (splitReadSet.size() < 1000) splitReadSet.insert(itOS->second); // Limit to at most 1000 split reads
	    }
	    totalSplitReadsAligned += splitReadSet.size();

	    // MSA
	    if (splitReadSet.size() > 1) svIt->srSupport = msa(splitReadSet, svIt->consensus);

	    // Search true split in candidates
	    if (!alignConsensus(c, *svIt, svRefStr, svType)) { svIt->consensus = ""; svIt->srSupport = 0; }
	  }
	}
      }
    }
  }
  kseq_destroy(seq);
  gzclose(fp);

  // Clean-up
  for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
    hts_idx_destroy(idx[file_c]);
    sam_close(samfile[file_c]);
  }

  return (totalSplitReadsAligned>0);
}


// Initialize clique, deletions
template<typename TBamRecordIterator, typename TSize>
inline void
_initClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<DeletionTag>) {
  svStart = el->mpos + el->malen;
  svEnd = el->pos;
  wiggle =  -el->maxNormalISize;
}

// Initialize clique, insertions
template<typename TBamRecordIterator, typename TSize>
inline void
_initClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<InsertionTag>) {
  svStart = el->mpos + el->malen;
  svEnd = el->pos;
  wiggle = -(svEnd - svStart);
}

// Initialize clique, duplications
template<typename TBamRecordIterator, typename TSize>
inline void
_initClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<DuplicationTag>) {
  svStart = el->mpos;
  svEnd = el->pos + el->alen;
  wiggle = el->maxNormalISize;
}

// Initialize clique, inversions
template<typename TBamRecordIterator, typename TSize>
inline void
_initClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<InversionTag>) {
  int ct=_getSpanOrientation(*el, el->libOrient, SVType<InversionTag>());
  if (!ct) {
    svStart = el->mpos + el->malen;
    svEnd = el->pos + el->alen;
  } else {
    svStart = el->mpos;
    svEnd = el->pos;
  }
  wiggle = el->maxNormalISize - std::max(el->alen, el->malen);
}

// Initialize clique, translocations
template<typename TBamRecordIterator, typename TSize>
inline void
_initClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<TranslocationTag>) {
  int ct=_getSpanOrientation(*el, el->libOrient, SVType<TranslocationTag>());
  if (ct%2==0) {
    svStart = el->pos + el->alen;
    if (ct>=2) svEnd = el->mpos;
    else svEnd = el->mpos + el->malen;
  } else {
    svStart = el->pos;
    if (ct>=2) svEnd = el->mpos + el->malen;
    else svEnd = el->mpos;
  }
  wiggle=el->maxNormalISize;
}


// Update clique, deletions
template<typename TBamRecordIterator, typename TSize>
inline bool 
_updateClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<DeletionTag>) 
{
  TSize newSvStart = std::max(svStart, el->mpos + el->malen);
  TSize newSvEnd = std::min(svEnd, el->pos);
  TSize newWiggle = el->pos + el->alen - el->mpos - el->maxNormalISize - (newSvEnd - newSvStart);
  TSize wiggleChange = wiggle + (svEnd-svStart) - (newSvEnd - newSvStart);
  if (wiggleChange > newWiggle) newWiggle=wiggleChange;

  // Does the new deletion size agree with all pairs
  if ((newSvStart < newSvEnd) && (newWiggle<=0)) {
    svStart = newSvStart;
    svEnd = newSvEnd;
    wiggle = newWiggle;
    return true;
  }
  return false;
}

// Update clique, insertions
template<typename TBamRecordIterator, typename TSize>
inline bool 
_updateClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<InsertionTag>) 
{
  TSize newSvStart = std::max(svStart, el->mpos + el->malen);
  TSize newSvEnd = std::min(svEnd, el->pos);
  TSize newWiggle = -(newSvEnd - newSvStart);

  // Does the new insertion size agree with all pairs
  if ((newSvStart < newSvEnd) && (newWiggle<=0)) {
    svStart = newSvStart;
    svEnd = newSvEnd;
    wiggle = newWiggle;
    return true;
  }
  return false;
}

// Update clique, duplications
template<typename TBamRecordIterator, typename TSize>
inline bool 
_updateClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<DuplicationTag>) 
{
  TSize newSvStart = std::min(svStart, el->mpos);
  TSize newSvEnd = std::max(svEnd, el->pos + el->alen);
  TSize newWiggle = el->pos - (el->mpos + el->malen) + el->maxNormalISize - (newSvEnd - newSvStart);
  TSize wiggleChange = wiggle - ((newSvEnd - newSvStart) - (svEnd-svStart));
  if (wiggleChange < newWiggle) newWiggle = wiggleChange;

  // Does the new duplication size agree with all pairs
  if ((newSvStart < newSvEnd) && (newWiggle>=0)) {
    svStart = newSvStart;
    svEnd = newSvEnd;
    wiggle = newWiggle;
    return true;
  }
  return false;
}

// Update clique, inversions
template<typename TBamRecordIterator, typename TSize>
inline bool 
_updateClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<InversionTag>) 
{
  int ct=_getSpanOrientation(*el, el->libOrient, SVType<InversionTag>());
  TSize newSvStart;
  TSize newSvEnd;
  TSize newWiggle;
  TSize wiggleChange;
  if (!ct) {
    newSvStart = std::max(svStart, el->mpos + el->malen);
    newSvEnd = std::max(svEnd, el->pos + el->alen);
    newWiggle = std::min(el->maxNormalISize - (newSvStart - el->mpos), el->maxNormalISize - (newSvEnd - el->pos));
    wiggleChange = wiggle - std::max(newSvStart - svStart, newSvEnd - svEnd);
  } else {
    newSvStart = std::min(svStart, el->mpos);
    newSvEnd = std::min(svEnd, el->pos);
    newWiggle = el->pos  + el->alen - (el->mpos + el->malen) + el->maxNormalISize - (newSvEnd - newSvStart);
    newWiggle = std::min(el->maxNormalISize - (el->mpos + el->malen - newSvStart), el->maxNormalISize - (el->pos + el->alen - newSvEnd));
    wiggleChange = wiggle - std::max(svStart - newSvStart, svEnd - newSvEnd);
  }
  if (wiggleChange < newWiggle) newWiggle=wiggleChange;

  // Does the new inversion size agree with all pairs
  if ((newSvStart < newSvEnd) && (newWiggle>=0)) {
    svStart = newSvStart;
    svEnd = newSvEnd;
    wiggle = newWiggle;
    return true;
  }
  return false;
}


// Update clique, translocations
template<typename TBamRecordIterator, typename TSize>
inline bool 
_updateClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<TranslocationTag>) 
{
  int ct = _getSpanOrientation(*el, el->libOrient, SVType<TranslocationTag>());
  TSize newSvStart;
  TSize newSvEnd;
  TSize newWiggle = wiggle;
  if (ct%2==0) {
    newSvStart = std::max(svStart, el->pos + el->alen);
    newWiggle -= (newSvStart - svStart);
    if (ct>=2) {
      newSvEnd = std::min(svEnd, el->mpos);
      newWiggle -= (svEnd - newSvEnd);
    } else  {
      newSvEnd = std::max(svEnd, el->mpos + el->malen);
      newWiggle -= (newSvEnd - svEnd);
    }
  } else {
    newSvStart = std::min(svStart, el->pos);
    newWiggle -= (svStart - newSvStart);
    if (ct>=2) {
      newSvEnd = std::max(svEnd, el->mpos + el->malen);
      newWiggle -= (newSvEnd - svEnd);
    } else {
      newSvEnd = std::min(svEnd, el->mpos);
      newWiggle -= (svEnd - newSvEnd);
    }
  }
  // Is this still a valid translocation cluster?
  if (newWiggle>0) {
    svStart = newSvStart;
    svEnd = newSvEnd;
    wiggle = newWiggle;
    return true;
  }
  return false;
}

template<typename TSize>
inline bool
_svSizeCheck(TSize const s, TSize const e, SVType<DeletionTag>) {
  return (( e - s ) >= 300);
}

template<typename TSize>
inline bool
_svSizeCheck(TSize const s, TSize const e, SVType<DuplicationTag>) {
  return (( e - s ) >= 100);
}

template<typename TSize>
inline bool
_svSizeCheck(TSize const s, TSize const e, SVType<InversionTag>) {
  return (( e - s ) >= 100);
}

template<typename TSize>
inline bool
_svSizeCheck(TSize const s, TSize const e, SVType<InsertionTag>) {
  return (( e - s ) >= 0);
}

template<typename TSize>
inline bool
_svSizeCheck(TSize const, TSize const, SVType<TranslocationTag>) {
  return true;
}


template<typename TConfig, typename TSampleLibrary, typename TSVs, typename TCountMap, typename TTag>
inline void
_annotateJunctionReads(TConfig const& c, TSampleLibrary& sampleLib, TSVs& svs, TCountMap& junctionCountMap, SVType<TTag> svType) 
{
  annotateJunctionReads(c.files, c.genome, c.minGenoQual, sampleLib, svs, junctionCountMap, svType);
}


template<typename TConfig, typename TRefNames, typename TSampleLibrary, typename TSVs, typename TCountMap, typename TTag>
inline void
_annotateCoverage(TConfig const& c, TRefNames const& refnames, TSampleLibrary& sampleLib, TSVs& svs, TCountMap& countMap, SVType<TTag>) 
{
  // Find Ns in the reference genome
  typedef boost::icl::interval_set<int> TNIntervals;
  typedef std::vector<TNIntervals> TNGenome;
  TNGenome ni;
  ni.resize(refnames.size());

  if (boost::filesystem::exists(c.genome) && boost::filesystem::is_regular_file(c.genome) && boost::filesystem::file_size(c.genome)) {
    kseq_t *seq;
    int l;
    gzFile fp = gzopen(c.genome.string().c_str(), "r");
    seq = kseq_init(fp);
    while ((l = kseq_read(seq)) >= 0) {
      for(int32_t refIndex=0; refIndex < (int32_t) refnames.size(); ++refIndex) {
	if (seq->name.s == refnames[refIndex]) {
	  bool nrun = false;
	  int nstart = l;
	  for(int i=0; i<l; ++i) {
	    if ((seq->seq.s[i] != 'n') && (seq->seq.s[i] != 'N')) {
	      if (nrun) {
		ni[refIndex].add(boost::icl::discrete_interval<int>::right_open(nstart,i));
		nrun = false;
	      }
	    } else {
	      if (!nrun) {
		nrun = true;
		nstart = i;
	      }
	    }
	  }
	  if (nrun) ni[refIndex].add(boost::icl::discrete_interval<int>::right_open(nstart,l));
	}
      }
    }
    kseq_destroy(seq);
    gzclose(fp);
  }

  // Add control regions
  TSVs svc = svs;
  unsigned int maxId = 0;
  for (typename TSVs::const_iterator itSV = svs.begin(); itSV != svs.end(); ++itSV)
    if (itSV->id > maxId) maxId = itSV->id;  
  // Assign control regions to primary SVs, true = left
  typedef std::pair<unsigned int, bool> TLR;
  typedef std::map<unsigned int, TLR> TSVMap;
  TSVMap svMap;
  for (typename TSVs::const_iterator itSV = svs.begin(); itSV != svs.end(); ++itSV) {
    int halfSize = (itSV->svEnd - itSV->svStart)/2;

    // Left control region
    StructuralVariantRecord sLeft;
    sLeft.chr = itSV->chr;
    sLeft.id = ++maxId;
    sLeft.svStart = std::max(itSV->svStart - halfSize, 0);
    sLeft.svEnd = itSV->svStart;
    typename TNIntervals::const_iterator itO = ni[sLeft.chr].find(boost::icl::discrete_interval<int>::right_open(sLeft.svStart, sLeft.svEnd));
    while (itO != ni[sLeft.chr].end()) {
      sLeft.svStart = std::max(itO->lower() - halfSize, 0);
      sLeft.svEnd = itO->lower();
      itO = ni[sLeft.chr].find(boost::icl::discrete_interval<int>::right_open(sLeft.svStart, sLeft.svEnd));
    }
    svMap.insert(std::make_pair(sLeft.id, std::make_pair(itSV->id, true)));
    svc.push_back(sLeft);

    // Right control region
    StructuralVariantRecord sRight;
    sRight.chr = itSV->chr;
    sRight.id = ++maxId;
    sRight.svStart = itSV->svEnd;
    sRight.svEnd = itSV->svEnd + halfSize;
    itO = ni[sRight.chr].find(boost::icl::discrete_interval<int>::right_open(sRight.svStart, sRight.svEnd));
    while (itO != ni[sRight.chr].end()) {
      sRight.svStart = itO->upper();
      sRight.svEnd = itO->upper() + halfSize;
      itO = ni[sRight.chr].find(boost::icl::discrete_interval<int>::right_open(sRight.svStart, sRight.svEnd));
    }
    svMap.insert(std::make_pair(sRight.id, std::make_pair(itSV->id, false)));
    svc.push_back(sRight);
    //std::cerr << itSV->id << ':' << sLeft.svStart << '-' << sLeft.svEnd << ',' << itSV->svStart << '-' << itSV->svEnd << ',' << sRight.svStart << '-' << sRight.svEnd << std::endl;
  }
  
  typedef std::pair<std::string, int> TSampleSVPair;
  typedef std::pair<int, int> TBpRead;
  typedef boost::unordered_map<TSampleSVPair, TBpRead> TReadCountMap;
  TReadCountMap readCountMap;
  annotateCoverage(c.files, c.minGenoQual, sampleLib, svc, readCountMap, BpLevelType<NoBpLevelCount>(), CoverageType<RedundancyFilterTag>());
  for (typename TReadCountMap::const_iterator rcIt = readCountMap.begin(); rcIt != readCountMap.end(); ++rcIt) {
    // Map control regions back to original id
    int svID = rcIt->first.second;
    typename TSVMap::const_iterator itSVMap = svMap.find(svID);
    if (itSVMap != svMap.end()) svID = itSVMap->second.first;
    typename TCountMap::iterator itCM = countMap.find(std::make_pair(rcIt->first.first, svID));
    if (itCM == countMap.end()) itCM = countMap.insert(std::make_pair(std::make_pair(rcIt->first.first, svID), ReadCount(0, 0, 0))).first;
    if (itSVMap == svMap.end()) itCM->second.rc = rcIt->second.second;
    else if (itSVMap->second.second) itCM->second.leftRC = rcIt->second.second;
    else itCM->second.rightRC = rcIt->second.second;
  }
}

template<typename TConfig, typename TRefNames, typename TSampleLibrary, typename TSVs, typename TCountMap>
inline void
_annotateCoverage(TConfig const&, TRefNames const&, TSampleLibrary&, TSVs&, TCountMap&, SVType<TranslocationTag>) 
{
  //Nop
}

template<typename TConfig, typename TSampleLibrary, typename TSVs, typename TCountMap, typename TTag>
inline void
_annotateSpanningCoverage(TConfig const& c, TSampleLibrary& sampleLib, TSVs& svs, TCountMap& spanCountMap, SVType<TTag> svType) 
{
  annotateSpanningCoverage(c.files, c.minGenoQual, sampleLib, svs, spanCountMap, svType);
}


template<typename TGraph, typename TWeightMap, typename TDumpFile, typename TRefLength, typename TRefNames, typename TSVs, typename TSVType>
inline void
_searchCliques(TGraph const& g, TWeightMap const& weightMap, bool const dumpPe, TDumpFile& dumpPeFile, TRefLength const& reflen, TRefNames const& refnames, TSVs& svs, unsigned int& clique_count, int const overallMaxISize, TSVType svType) {
  // Compute the connected components
  std::vector<int> my_comp(num_vertices(g));
  int numComp = boost::connected_components(g, &my_comp[0]);
    
  // Count the number of vertices for each component
  typedef std::vector<unsigned int> TCompSize;
  TCompSize compSize(numComp);
  std::fill(compSize.begin(), compSize.end(), 0);
  typename boost::graph_traits<TGraph>::vertex_iterator vIt, vItEnd;
  for(boost::tie(vIt, vItEnd) = boost::vertices(g); vIt != vItEnd; ++vIt) ++compSize[my_comp[*vIt]];

  // Iterate each component
#pragma omp parallel for default(shared)
  for(int compIt = 0; compIt < numComp; ++compIt) {
    if (compSize[compIt]<2) continue;
    typedef typename boost::graph_traits<TGraph>::vertex_descriptor TVertexDescriptor;
    typedef EdgeRecord<unsigned short, TVertexDescriptor> TEdgeRecord;
    typedef std::vector<TEdgeRecord> TWeightEdge;
    TWeightEdge wEdge;
    typename boost::graph_traits<TGraph>::edge_iterator eIt, eItEnd;
    for(boost::tie(eIt, eItEnd) = boost::edges(g); eIt != eItEnd; ++eIt) {
      if ((my_comp[boost::source(*eIt, g)] == compIt) && (my_comp[boost::source(*eIt, g)] == my_comp[boost::target(*eIt,g)])) {
	wEdge.push_back(TEdgeRecord(boost::source(*eIt, g), boost::target(*eIt, g), weightMap[*eIt]));
      }
    }
    
    // Sort edges by weight
    std::sort(wEdge.begin(), wEdge.end(), SortEdgeRecords<TEdgeRecord>());
    //for(TWeightEdge::const_iterator itWEdge = wEdge.begin(); itWEdge!=wEdge.end(); ++itWEdge) std::cerr << refIndex << ',' << compIt << ',' << itWEdge->source << ',' << itWEdge->target << ',' << itWEdge->weight << std::endl;
      
    // Find a large clique
    typename TWeightEdge::const_iterator itWEdge = wEdge.begin();
    typename TWeightEdge::const_iterator itWEdgeEnd = wEdge.end();
    typedef std::set<TVertexDescriptor> TCliqueMembers;
    TCliqueMembers clique;
    TCliqueMembers incompatible;
    int svStart, svEnd, wiggle;
    int32_t clusterRefID=g[itWEdge->source]->tid;
    int32_t clusterMateRefID=g[itWEdge->source]->mtid;
    _initClique(g[itWEdge->source], svStart, svEnd, wiggle, svType);
    int connectionType = _getSpanOrientation(*g[itWEdge->source], g[itWEdge->source]->libOrient, svType);
    if ((clusterRefID==clusterMateRefID) && (svStart >= svEnd))  continue;
    clique.insert(itWEdge->source);
    
    // Grow the clique from the seeding edge
    bool cliqueGrow=true;
    while ((cliqueGrow) && (clique.size() < compSize[compIt])) {
      itWEdge = wEdge.begin();
      cliqueGrow = false;
      for(;(!cliqueGrow) && (itWEdge != itWEdgeEnd);++itWEdge) {
	TVertexDescriptor v;
	if ((clique.find(itWEdge->source) == clique.end()) && (clique.find(itWEdge->target) != clique.end())) v = itWEdge->source;
	else if ((clique.find(itWEdge->source) != clique.end()) && (clique.find(itWEdge->target) == clique.end())) v = itWEdge->target;
	else continue;
	if (incompatible.find(v) != incompatible.end()) continue;
	typename boost::graph_traits<TGraph>::adjacency_iterator vi, vi_end;
	unsigned int cliqSize = 0;
	for(boost::tie(vi, vi_end) = boost::adjacent_vertices(v, g); vi != vi_end; ++vi)
	  if (clique.find(*vi) != clique.end()) ++cliqSize;
	if (cliqSize == clique.size()) {
	  //std::cerr << refIndex << ',' << compIt << ',' << v << ',' << svStart << ',' << svEnd << ',' << wiggle << std::endl;
	  cliqueGrow = _updateClique(g[v], svStart, svEnd, wiggle, svType);
	  if (cliqueGrow) clique.insert(v);
	  else incompatible.insert(v);
	}
      }
    }

    if ((clique.size()>1) && (_svSizeCheck(svStart, svEnd, svType))) {
      StructuralVariantRecord svRec;
      svRec.chr = clusterRefID;
      svRec.chr2 = clusterMateRefID;
      svRec.svStartBeg = std::max((int) svStart - overallMaxISize, 0);
      svRec.svStart = std::min((uint32_t) svStart + 1, reflen[clusterRefID]);
      svRec.svStartEnd = std::min((uint32_t) svStart + overallMaxISize, reflen[clusterRefID]);
      svRec.svEndBeg = std::max((int) svEnd - overallMaxISize, 0);
      svRec.svEnd = std::min((uint32_t) svEnd+1, reflen[clusterMateRefID]);
      svRec.svEndEnd = std::min((uint32_t) svEnd + overallMaxISize, reflen[clusterMateRefID]);
      svRec.peSupport = clique.size();
      svRec.wiggle = abs(wiggle);
      std::vector<uint8_t> mapQV;
      for(typename TCliqueMembers::const_iterator itC = clique.begin(); itC!=clique.end(); ++itC) mapQV.push_back(g[*itC]->MapQuality);
      std::sort(mapQV.begin(), mapQV.end());
      svRec.peMapQuality = mapQV[mapQV.size()/2];
      if ((clusterRefID==clusterMateRefID) && (svRec.svStartEnd > svRec.svEndBeg)) {
	unsigned int midPointDel = ((svRec.svEnd - svRec.svStart) / 2) + svRec.svStart;
	svRec.svStartEnd = midPointDel -1;
	svRec.svEndBeg = midPointDel;
      }
      svRec.srSupport=0;
      svRec.srAlignQuality=0;
      svRec.precise=false;
      svRec.ct=connectionType;
      std::vector<int32_t> inslenV;
      for(typename TCliqueMembers::const_iterator itC = clique.begin(); itC!=clique.end(); ++itC) inslenV.push_back(g[*itC]->Median - (abs(g[*itC]->pos - g[*itC]->mpos) + g[*itC]->alen));
      std::sort(inslenV.begin(), inslenV.end());
      svRec.insLen = inslenV[inslenV.size()/2];
#pragma omp critical
      {
	svRec.id = clique_count++;
	svs.push_back(svRec);
      }
      
      // Dump PEs
      if (dumpPe) {
#pragma omp critical
	{
	  for(typename TCliqueMembers::const_iterator itC=clique.begin(); itC!=clique.end(); ++itC) {
	    std::stringstream id;
	    id << _addID(svType) << std::setw(8) << std::setfill('0') << svRec.id;
	    dumpPeFile << id.str() << "\t" << refnames[g[*itC]->tid] << "\t" << g[*itC]->pos << "\t" <<  refnames[g[*itC]->mtid] << "\t" << g[*itC]->mpos << "\t" << (unsigned int) g[*itC]->MapQuality << std::endl;
	  }
	}
      }
    }
  }
}

template<typename TSVType>
inline int run(Config const& c, TSVType svType) {
#ifdef PROFILE
  ProfilerStart("delly.prof");
#endif
  
  // Collect all promising structural variants
  typedef std::vector<StructuralVariantRecord> TVariants;
  TVariants svs;

  // Clique id counter
  unsigned int clique_count = 1;

  // Create library objects
  typedef boost::unordered_map<std::string, LibraryInfo> TLibraryMap;
  typedef boost::unordered_map<std::string, TLibraryMap> TSampleLibrary;
  TSampleLibrary sampleLib;
  int overallMaxISize = 0;
  getLibraryParams(c.files, sampleLib, c.percentAbnormal, c.madCutoff);
  for(TSampleLibrary::const_iterator sampleIt=sampleLib.begin(); sampleIt!=sampleLib.end();++sampleIt)
    for(TLibraryMap::const_iterator libIt=sampleIt->second.begin();libIt!=sampleIt->second.end();++libIt)
      if (libIt->second.maxNormalISize > overallMaxISize) overallMaxISize = libIt->second.maxNormalISize;

  // Open file handles
  typedef std::vector<std::string> TRefNames;
  typedef std::vector<uint32_t> TRefLength;
  TRefNames refnames;
  TRefLength reflen;
  typedef std::vector<samFile*> TSamFile;
  typedef std::vector<hts_idx_t*> TIndex;
  TSamFile samfile;
  TIndex idx;
  samfile.resize(c.files.size());
  idx.resize(c.files.size());
  for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
    samfile[file_c] = sam_open(c.files[file_c].string().c_str(), "r");
    idx[file_c] = sam_index_load(samfile[file_c], c.files[file_c].string().c_str());
    if (!file_c) {
      bam_hdr_t* hdr = sam_hdr_read(samfile[file_c]);
      for (int i = 0; i<hdr->n_targets; ++i) {
	refnames.push_back(hdr->target_name[i]);
	reflen.push_back(hdr->target_len[i]);
      }
      bam_hdr_destroy(hdr);
    }
  }

  // Dump PE
  bool dumpPe=false;
  std::ofstream dumpPeFile;
  if (c.peDump.string().size()) {
    dumpPe=true;
    dumpPeFile.open(c.peDump.string().c_str());
    dumpPeFile << "#id\tchr\tpos\tmatechr\tmatepos\tmapq" << std::endl;
  }

  // Parse exclude interval list
  TRefNames::const_iterator itRef = refnames.begin();
  std::vector<bool> validChr;
  typedef std::vector<ExcludeInterval> TExclInterval;
  TExclInterval exclIntervals;
  validChr.resize(refnames.size(), true);
  if (boost::filesystem::exists(c.exclude) && boost::filesystem::is_regular_file(c.exclude) && boost::filesystem::file_size(c.exclude)) {
    typedef boost::unordered_map<std::string, unsigned int> TMapChr;
    TMapChr mapChr;
    for(unsigned int i = 0;itRef!=refnames.end();++itRef, ++i) mapChr[ *itRef ] = i;
    std::ifstream chrFile(c.exclude.string().c_str(), std::ifstream::in);
    if (chrFile.is_open()) {
      while (chrFile.good()) {
	std::string chrFromFile;
	getline(chrFile, chrFromFile);
	typedef boost::tokenizer< boost::char_separator<char> > Tokenizer;
	boost::char_separator<char> sep(" \t,;");
	Tokenizer tokens(chrFromFile, sep);
	Tokenizer::iterator tokIter = tokens.begin();
	if (tokIter!=tokens.end()) {
	  std::string chr=*tokIter++;
	  TMapChr::const_iterator mapChrIt = mapChr.find(chr);
	  if (mapChrIt != mapChr.end()) {
	    if (tokIter!=tokens.end()) {
	      int32_t start = boost::lexical_cast<int32_t>(*tokIter++);
	      int32_t end = boost::lexical_cast<int32_t>(*tokIter++);
	      exclIntervals.push_back(ExcludeInterval(mapChrIt->second, start, end));
	    } else validChr[mapChrIt->second]=false; // Exclude entire chromosome
	  }
	}
      }
      chrFile.close();
    }
  }
  std::sort(exclIntervals.begin(), exclIntervals.end(), SortExcludeIntervals<ExcludeInterval>());

  // Do we have an input vcffile 
  bool peMapping=true;
  if (boost::filesystem::exists(c.vcffile) && boost::filesystem::is_regular_file(c.vcffile) && boost::filesystem::file_size(c.vcffile)) peMapping=false;

  // Qualities
  typedef boost::unordered_map<std::size_t, uint8_t> TQualities;
  std::vector<TQualities> qualities;
  qualities.resize(c.files.size());
  typedef boost::unordered_map<std::size_t, int32_t> TAlignmentLength;
  std::vector<TAlignmentLength> alen;
  alen.resize(c.files.size());

  // Process chromosome by chromosome
  boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Paired-end clustering" << std::endl;
  boost::progress_display show_progress( refnames.size() );
  for(int refIndex=0; (refIndex < (int) refnames.size()) && (peMapping); ++refIndex) {
    ++show_progress;
    if (!validChr[refIndex]) continue;
      
    // Create bam alignment record vector
    typedef std::vector<BamAlignRecord> TBamRecord;
    TBamRecord bamRecord;

    // Iterate all samples
#pragma omp parallel for default(shared)
    for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
      // Get a sample name
      std::string sampleName(c.files[file_c].stem().string());
      TSampleLibrary::iterator sampleIt=sampleLib.find(sampleName);

      // Unique pairs for the given sample
      typedef boost::container::flat_set<int32_t> TUniquePairs;
      TUniquePairs unique_pairs;

      // Read alignments
      int32_t oldAlignPos=-1;
      hts_itr_t* iter = sam_itr_queryi(idx[file_c], refIndex, 0, reflen[refIndex]);
      bam1_t* rec = bam_init1();
      while (sam_itr_next(samfile[file_c], iter, rec) >= 0) {
	if (rec->core.flag & (BAM_FSECONDARY | BAM_FQCFAIL | BAM_FDUP | BAM_FSUPPLEMENTARY | BAM_FUNMAP | BAM_FMUNMAP)) continue;
	if ((rec->core.flag & BAM_FPAIRED) && (rec->core.qual >= c.minMapQual) && (rec->core.tid>=0) && (rec->core.mtid>=0)) {
	  // Mapping positions valid?
	  if (_mappingPos(rec->core.tid, rec->core.mtid, rec->core.pos, rec->core.mpos, svType)) continue;
	
	  // Is the read or its mate in a black-masked region
	  if (!exclIntervals.empty()) {
	    typename TExclInterval::const_iterator itBlackMask = std::lower_bound(exclIntervals.begin(), exclIntervals.end(), ExcludeInterval(rec->core.tid, rec->core.pos, 0), SortExcludeIntervals<ExcludeInterval>());
	    if (itBlackMask!=exclIntervals.begin()) --itBlackMask;
	    if ((itBlackMask->tid==rec->core.tid) && (itBlackMask->start <= rec->core.pos) && (rec->core.pos<=itBlackMask->end)) continue;
	    itBlackMask = std::lower_bound(exclIntervals.begin(), exclIntervals.end(), ExcludeInterval(rec->core.mtid, rec->core.mpos, 0), SortExcludeIntervals<ExcludeInterval>());
	    if (itBlackMask!=exclIntervals.begin()) --itBlackMask;
	    if ((itBlackMask->tid==rec->core.mtid) && (itBlackMask->start <= rec->core.mpos) && (rec->core.mpos<=itBlackMask->end)) continue;
	  }
	  
	  // Is this a discordantly mapped paired-end?
	  std::string rG = "DefaultLib";
	  uint8_t *rgptr = bam_aux_get(rec, "RG");
	  if (rgptr) {
	    char* rg = (char*) (rgptr + 1);
	    rG = std::string(rg);
	  }
	  TLibraryMap::iterator libIt=sampleIt->second.find(rG);
	  if (libIt==sampleIt->second.end()) std::cerr << "Missing read group: " << rG << std::endl;
	  if (libIt->second.median == 0) continue; // Single-end library
	  if (_acceptedInsertSize(libIt->second, abs(rec->core.isize), svType)) continue;  // Normal paired-end (for deletions/insertions only)
	  if (_acceptedOrientation(libIt->second.defaultOrient, getStrandIndependentOrientation(rec->core), svType)) continue;  // Orientation disagrees with SV type
	  
	  // Get or store the mapping quality for the partner
	  if (_firstPairObs(rec->core.tid, rec->core.mtid, rec->core.pos, rec->core.mpos, svType)) {
	    uint8_t r2Qual = rec->core.qual;
	    uint8_t* ptr = bam_aux_get(rec, "AS");
	    if (ptr) {
	      int score = std::abs((int) bam_aux2i(ptr));
	      r2Qual = std::min(r2Qual, (uint8_t) ( (score<255) ? score : 255 ));
	    }
	    std::size_t hv = hash_pair(rec);
	    qualities[file_c][hv]= r2Qual;
	    alen[file_c][hv]= alignmentLength(rec);
	  } else {
	    // Get the two mapping qualities
	    uint8_t r2Qual = rec->core.qual;
	    uint8_t* ptr = bam_aux_get(rec, "AS");
	    if (ptr) {
	      int score = std::abs((int) bam_aux2i(ptr));
	      r2Qual = std::min(r2Qual, (uint8_t) ( (score<255) ? score : 255 ));
	    }
	    std::size_t hv=hash_pair_mate(rec);
	    uint8_t pairQuality = std::min(qualities[file_c][hv], r2Qual);
	    qualities[file_c][hv]= (uint8_t) 0;
	    
	    // Pair quality
	    if (pairQuality < c.minMapQual) continue;
	    
	    // Store the paired-end
	    if (rec->core.pos!=oldAlignPos) {
	      oldAlignPos=rec->core.pos;
	      unique_pairs.clear();
	    }
	    if (unique_pairs.insert(rec->core.mpos).second) {
#pragma omp critical
	      {
		bamRecord.push_back(BamAlignRecord(rec, pairQuality, alignmentLength(rec), alen[file_c][hv], libIt->second.median, libIt->second.mad, libIt->second.maxNormalISize, libIt->second.defaultOrient));
	      }
	      ++libIt->second.abnormal_pairs;
	    } else {
	      ++libIt->second.non_unique_abnormal_pairs;
	    }
	  }
	}
      }
      // Clean-up qualities
      _resetQualities(qualities[file_c], alen[file_c], svType);

      bam_destroy1(rec);
      hts_itr_destroy(iter);
    }
    
    // Sort BAM records according to position
    std::sort(bamRecord.begin(), bamRecord.end(), SortBamRecords<BamAlignRecord>());
    //for(TBamRecord::const_iterator bamIt = bamRecord.begin(); bamIt!=bamRecord.end(); ++bamIt) std::cerr << bamIt->tid << ',' << bamIt->pos << ',' << bamIt->mtid << ',' << bamIt->mpos << std::endl;


    // Define an undirected graph g
    typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS, TBamRecord::const_iterator, boost::property<boost::edge_weight_t, unsigned int> > Graph;
    Graph g;
      
    // Define the reverse map
    typedef boost::graph_traits<Graph>::vertex_descriptor Vertex;
    typedef boost::unordered_map<unsigned int, Vertex> TNameVertexMap;
    TNameVertexMap nameFrag;
      
    // Define the edge property map
    typedef boost::property_map<Graph, boost::edge_weight_t>::type edge_map_t;
    edge_map_t weightMap = get(boost::edge_weight, g);
      
    // Iterate the chromosome range
    unsigned int lastConnectedNode = 0;
    TBamRecord::const_iterator vecBeg = bamRecord.begin();
    TBamRecord::const_iterator vecEnd = bamRecord.end();
    for(;vecBeg!=vecEnd; ++vecBeg) {
      // Safe to clean the graph?
      if ( (vecBeg-bamRecord.begin()) > lastConnectedNode) {
	if (boost::num_vertices(g)) {
	  _searchCliques(g, weightMap, dumpPe, dumpPeFile, reflen, refnames, svs, clique_count, overallMaxISize, svType);
	  g.clear();
	  nameFrag.clear();
	}
      }
      int32_t const minCoord = _minCoord(vecBeg->pos, vecBeg->mpos, svType);
      int32_t const maxCoord = _maxCoord(vecBeg->pos, vecBeg->mpos, svType);
      TBamRecord::const_iterator vecNext = vecBeg + 1;
      for(; ((vecNext != vecEnd) && (abs(_minCoord(vecNext->pos, vecNext->mpos, svType) + vecNext->alen - minCoord) <= overallMaxISize)) ; ++vecNext) {
	// Check that mate chr agree (only for translocations)
	if (vecBeg->mtid!=vecNext->mtid) continue;

	// Check combinability of pairs
	if (_pairsDisagree(minCoord, maxCoord, vecBeg->alen, vecBeg->maxNormalISize, _minCoord(vecNext->pos, vecNext->mpos, svType), _maxCoord(vecNext->pos, vecNext->mpos, svType), vecNext->alen, vecNext->maxNormalISize, _getSpanOrientation(*vecBeg, vecBeg->libOrient, svType), _getSpanOrientation(*vecNext, vecNext->libOrient, svType), svType)) continue;

	// Update last connected node
	if ( (vecNext-bamRecord.begin()) > lastConnectedNode ) lastConnectedNode = vecNext-bamRecord.begin();

	TNameVertexMap::iterator pos;
	bool inserted;
	
	// Add vertex 1
	Vertex u;
	boost::tie(pos, inserted) = nameFrag.insert(TNameVertexMap::value_type(vecBeg-bamRecord.begin(), Vertex()));
	if (inserted) {
	  u = add_vertex(g);
	  pos->second = u;
	  g[u]=vecBeg;
	} else {
	  u = pos->second;
	}
	
	// Add vertex 2
	Vertex v;
	boost::tie(pos, inserted) = nameFrag.insert(TNameVertexMap::value_type(vecNext-bamRecord.begin(), Vertex()));
	if (inserted) {
	  v = add_vertex(g);
	  pos->second = v;
	  g[v]=vecNext;
	} else {
	  v = pos->second;
	}
	
	// Add the edge
	if ((out_degree(u, g) <= c.graphPruning) || (out_degree(v, g) <= c.graphPruning)) {
	  boost::graph_traits<Graph>::edge_descriptor e;
	  tie(e, inserted) = add_edge(u,v, g);
	  if (inserted) weightMap[e] = abs( abs( (_minCoord(vecNext->pos, vecNext->mpos, svType) - minCoord) - (_maxCoord(vecNext->pos, vecNext->mpos, svType) - maxCoord) ) - abs(vecBeg->Median - vecNext->Median) );
	}
      }
    }
    if (boost::num_vertices(g)) {
      _searchCliques(g, weightMap, dumpPe, dumpPeFile, reflen, refnames, svs, clique_count, overallMaxISize, svType);
      g.clear();
      nameFrag.clear();
    }
  }  
  
  // Close dump PE file
  if (dumpPe) dumpPeFile.close();

  // Clean-up
  for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
    hts_idx_destroy(idx[file_c]);
    sam_close(samfile[file_c]);
  }

  // Split-read search
  if (peMapping) {
    if (boost::filesystem::exists(c.genome) && boost::filesystem::is_regular_file(c.genome) && boost::filesystem::file_size(c.genome)) 
      if (!svs.empty()) 
	findPutativeSplitReads(c, svs, svType);
  } else {
    // Read SV records from input vcffile
    vcfParse(c, refnames, reflen, overallMaxISize, svs, svType);
  }

  // Debug output
  //for (TVariants::const_iterator s = svs.begin();s!=svs.end();++s) std::cerr << s->svStart << ',' << s->svEnd << ',' <<  s->svStartBeg << ',' << s->svStartEnd << ',' << s->svEndBeg << ',' << s->svEndEnd << ',' << s->peSupport << ',' << s->srSupport << ',' << s->wiggle << ',' << s->srAlignQuality << ',' << s->precise << ',' << s->ct << ',' << s->peMapQuality << ',' << s->chr << ',' << s->chr2 << ',' << s->consensus << std::endl;

  // Any SVs for genotyping
  if (svs.empty()) {
    std::cout << "No structural variants found!" << std::endl;
    std::cout << "Done." << std::endl;
    return 0;
  }

  // Annotate junction reads
  typedef std::pair<std::string, int> TSampleSVPair;
  typedef std::pair<std::vector<uint8_t>, std::vector<uint8_t> > TReadQual;
  typedef boost::unordered_map<TSampleSVPair, TReadQual> TJunctionCountMap;
  TJunctionCountMap junctionCountMap;
  if (boost::filesystem::exists(c.genome) && boost::filesystem::is_regular_file(c.genome) && boost::filesystem::file_size(c.genome)) 
    _annotateJunctionReads(c, sampleLib, svs, junctionCountMap, svType);

  // Annotate spanning coverage
  typedef boost::unordered_map<TSampleSVPair, TReadQual> TCountMap;
  TCountMap spanCountMap;
  _annotateSpanningCoverage(c, sampleLib, svs, spanCountMap, svType);

  // Annotate coverage
  typedef boost::unordered_map<TSampleSVPair, ReadCount> TRCMap;
  TRCMap rcMap;
  _annotateCoverage(c, refnames, sampleLib, svs, rcMap, svType);

  // VCF output
  if (svs.size()) {
    vcfOutput(c, svs, junctionCountMap, rcMap, spanCountMap, svType);
  }


  // Output library statistics
  now = boost::posix_time::second_clock::local_time();
  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Library statistics" << std::endl;
  TSampleLibrary::const_iterator sampleIt=sampleLib.begin();
  for(;sampleIt!=sampleLib.end();++sampleIt) {
    std::cout << "Sample: " << sampleIt->first << std::endl;
    TLibraryMap::const_iterator libIt=sampleIt->second.begin();
    for(;libIt!=sampleIt->second.end();++libIt) {
      std::cout << "RG: ID=" << libIt->first << ",Median=" << libIt->second.median << ",MAD=" << libIt->second.mad << ",Orientation=" << (int) libIt->second.defaultOrient << ",InsertSizeCutoff=" << libIt->second.maxNormalISize << ",DuplicateDiscordantPairs=" << libIt->second.non_unique_abnormal_pairs << ",UniqueDiscordantPairs=" << libIt->second.abnormal_pairs << std::endl;
    }
  }
  
#ifdef PROFILE
  ProfilerStop();
#endif

  // End
  now = boost::posix_time::second_clock::local_time();
  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] Done." << std::endl;;
  return 0;
}


int main(int argc, char **argv) {
  Config c;

  // Define generic options
  boost::program_options::options_description generic("Generic options");
  generic.add_options()
    ("help,?", "show help message")
    ("type,t", boost::program_options::value<std::string>(&c.svType)->default_value("DEL"), "SV analysis type (DEL, DUP, INV, TRA)")
    ("outfile,o", boost::program_options::value<boost::filesystem::path>(&c.outfile)->default_value("sv.vcf"), "SV output file")
    ("exclude,x", boost::program_options::value<boost::filesystem::path>(&c.exclude)->default_value(""), "file with chr to exclude")
    ;

  boost::program_options::options_description pem("PE options");
  pem.add_options()
    ("map-qual,q", boost::program_options::value<unsigned short>(&c.minMapQual)->default_value(1), "min. paired-end mapping quality")
    ("mad-cutoff,s", boost::program_options::value<unsigned short>(&c.madCutoff)->default_value(9), "insert size cutoff, median+s*MAD (deletions only)")
    ;

  boost::program_options::options_description breaks("SR options");
  breaks.add_options()
    ("genome,g", boost::program_options::value<boost::filesystem::path>(&c.genome), "genome fasta file")
    ("min-flank,m", boost::program_options::value<unsigned int>(&c.minimumFlankSize)->default_value(13), "minimum flanking sequence size")
    ;

  boost::program_options::options_description geno("Genotyping options");
  geno.add_options()
    ("vcfgeno,v", boost::program_options::value<boost::filesystem::path>(&c.vcffile)->default_value("site.vcf"), "input vcf file for genotyping only")
    ("geno-qual,u", boost::program_options::value<unsigned short>(&c.minGenoQual)->default_value(5), "min. mapping quality for genotyping")
    ;

  // Define hidden options
  boost::program_options::options_description hidden("Hidden options");
  hidden.add_options()
    ("input-file", boost::program_options::value< std::vector<boost::filesystem::path> >(&c.files), "input file")
    ("pe-dump,p", boost::program_options::value<boost::filesystem::path>(&c.peDump)->default_value(""), "outfile to dump PE info")
    ("pe-fraction,c", boost::program_options::value<float>(&c.percentAbnormal)->default_value(0.0), "fixed fraction c of discordant PEs, for c=0 MAD cutoff is used")
    ("flanking,f", boost::program_options::value<unsigned int>(&c.flankQuality)->default_value(80), "quality of the aligned flanking region")
    ("pruning,j", boost::program_options::value<unsigned int>(&c.graphPruning)->default_value(250), "PE graph pruning cutoff")
    ("warranty,w", "show warranty")
    ("license,l", "show license")
    ;

  boost::program_options::positional_options_description pos_args;
  pos_args.add("input-file", -1);

  // Set the visibility
  boost::program_options::options_description cmdline_options;
  cmdline_options.add(generic).add(pem).add(breaks).add(geno).add(hidden);
  boost::program_options::options_description visible_options;
  visible_options.add(generic).add(pem).add(breaks).add(geno);
  boost::program_options::variables_map vm;
  boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(cmdline_options).positional(pos_args).run(), vm);
  boost::program_options::notify(vm);


  // Check command line arguments
  if ((vm.count("help")) || (!vm.count("input-file"))) { 
    printTitle("DELLY");
    if (vm.count("warranty")) {
      displayWarranty();
    } else if (vm.count("license")) {
      gplV3();
    } else {
      std::cout << "Usage: " << argv[0] << " [OPTIONS] <sample1.sort.bam> <sample2.sort.bam> ..." << std::endl;
      std::cout << visible_options << "\n"; 
    }
    return 1; 
  }

  // Show cmd
  boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] ";
  for(int i=0; i<argc; ++i) { std::cout << argv[i] << ' '; }
  std::cout << std::endl;

  // Always ignore reads of mapping quality <5 for genotyping, otherwise het. is more likely!
  if (c.minGenoQual<5) c.minGenoQual=5;

  // Run main program
  if (c.svType == "DEL") return run(c, SVType<DeletionTag>());
  else if (c.svType == "DUP") return run(c, SVType<DuplicationTag>());
  else if (c.svType == "INV") return run(c, SVType<InversionTag>());
  else if (c.svType == "TRA") return run(c, SVType<TranslocationTag>());
  else if (c.svType == "INS") return run(c, SVType<InsertionTag>());
  else {
    std::cerr << "SV analysis type not supported by Delly: " << c.svType << std::endl;
    return -1;
  }
  
}
