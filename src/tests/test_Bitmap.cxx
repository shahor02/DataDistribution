// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#define BOOST_TEST_MAIN
#define BOOST_TEST_MODULE "Bitmap"

#include <boost/test/unit_test.hpp>
#include <DataDistLogger.h>

#include <iostream>

using namespace o2::DataDistribution;



BOOST_AUTO_TEST_CASE(GetNextSeqNameTest)
{

  std::cout << sizeof (TokenBitfield<220>) << " " << sizeof (TokenBitfield<256>) << std::endl;

  {
    TokenBitfield<220> lField1;
    TokenBitfield<220> lField2;

    BOOST_CHECK(lField1.empty());
    BOOST_CHECK(lField2.empty());

    lField1.set_all();
    BOOST_CHECK(!lField1.empty());

    BOOST_CHECK(lField1.first() == 1);
    BOOST_CHECK(lField2.first() == TokenBitfield<220>::sInvalidIdx);


    lField2.set(23);
    lField1 &= lField2;
    BOOST_CHECK(lField1.first() == 23);
  }

  {
    TokenBitfield<256> lField1;
    TokenBitfield<256> lField2;

    lField1.set(73);
    lField1.set(53);

    lField2.set(23);
    lField2.set(73);
    lField2 &= lField1;
    BOOST_CHECK(lField2.first() == 73);
    BOOST_CHECK(lField2.random_idx(0) == 73);
    BOOST_CHECK(lField2.random_idx(1) == 73);
    BOOST_CHECK(lField2.random_idx(2) == 73);
    BOOST_CHECK(lField2.random_idx(3) == 73);
    BOOST_CHECK(lField2.random_idx(443) == 73);
    BOOST_CHECK(lField2.random_idx(2222) == 73);

    lField2.clr(73);
    BOOST_CHECK(lField2.empty());
  }


  // std::cerr << format(FmtSubSpec, 0) << std::endl;
  // DataDistLogger(DataDistSeverity::info, DataDistLogger::log_fmq{}, std::string("{}"));
}
