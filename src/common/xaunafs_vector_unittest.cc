/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"
#include "common/xaunafs_vector.h"

#include <gtest/gtest.h>

#include "unittests/inout_pair.h"

TEST(XaunaFSVectorTest, GeneralBehaviour) {
	XaunaFSVector<int>    vec1_A;
	std::vector<int>      vec1_B;
	EXPECT_EQ(vec1_A, vec1_B);

	XaunaFSVector<double> vec2_A(5, 1.0);
	std::vector<double>   vec2_B(5, 1.0);
	EXPECT_EQ(vec2_A, vec2_B);

	XaunaFSVector<double> vec3_A(vec2_B);
	std::vector<double>   vec3_B(vec2_A);
	EXPECT_EQ(vec2_B, vec3_A);
	EXPECT_EQ(vec2_A, vec3_B);

	vec1_A.push_back(5);
	EXPECT_NE(vec1_A, vec1_B);
	vec2_A[0] = 2.0;
	EXPECT_NE(vec2_A, vec2_B);
}

TEST(XaunaFSVectorTest, Serialization) {
	typedef std::vector<uint16_t>   OrdinaryVec;
	typedef XaunaFSVector<uint16_t> XaunaFsVec;

	OrdinaryVec o1 {1, 20, 300, 400};
	OrdinaryVec o2 (o1);
	XaunaFsVec  m1 (o1);
	XaunaFsVec  m2 (o1);

	std::vector<uint8_t> o_buffers[2];
	serialize(o_buffers[0], o1);
	serialize(o_buffers[1], o2);

	std::vector<uint8_t> m_buffers[2];
	serialize(m_buffers[0], m1);
	serialize(m_buffers[1], m2);

	// Check if XaunaFSVector is serialized differently then std::vector:
	ASSERT_NE(o_buffers[0], m_buffers[0]);

	// Check if XaunaFSVectors is always serialized the same way:
	ASSERT_EQ(m_buffers[0], m_buffers[1]);
	// Same about std::vector:
	ASSERT_EQ(o_buffers[0], o_buffers[1]);

	XaunaFsVec m1_deserialized;
	ASSERT_NE(m1, m1_deserialized);
	deserialize(m_buffers[0], m1_deserialized);
	ASSERT_EQ(m1, m1_deserialized);
}
