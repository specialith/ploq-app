/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <gtest/gtest.h>

#include <cstring>

#include "io/file.h"

#include "global/serialization/zipwriter.h"
#include "global/serialization/zipreader.h"

using namespace muse;

class Zip_RW_Tests : public ::testing::Test
{
public:
};

TEST_F(Zip_RW_Tests, Write_And_Read)
{
    //! [GIVEN] A zip file
    io::IODevice* device = new io::File("test.zip");
    ZipWriter writer(device);

    //! [WHEN] Writing data to the zip file
    writer.addFile("file1.txt", "Hello World!");
    writer.addFile("folder/file2.txt", "Hello World 2!");

    writer.close();

    //! [THEN] The data can be read back
    ZipReader reader(device);
    auto data = reader.fileData("file1.txt");
    EXPECT_EQ(data, "Hello World!");

    auto data2 = reader.fileData("folder/file2.txt");
    EXPECT_EQ(data2, "Hello World 2!");

    reader.close();
}

TEST_F(Zip_RW_Tests, Refuses_Entries_That_Expand_Too_Far)
{
    //! [GIVEN] A zip whose entry is 40 MB of zeros (a few kilobytes compressed: a zip bomb in miniature),
    //! next to an ordinary entry and a 4 MB entry that compresses normally
    ByteArray zeros(40 * 1024 * 1024);
    std::memset(zeros.data(), 0, zeros.size());
    ByteArray varied(4 * 1024 * 1024);
    uint32_t x = 12345;
    for (size_t i = 0; i < varied.size(); ++i) {
        x = x * 1103515245u + 12345u;
        varied.data()[i] = static_cast<uint8_t>('a' + ((x >> 16) % 8)); // compresses about 3:1
    }
    io::IODevice* device = new io::File("bomb.zip"); // not deleted here: writer and reader use it until they go
    ZipWriter writer(device);
    writer.addFile("bomb.bin", zeros);
    writer.addFile("score.mscx", "<museScore/>");
    writer.addFile("big.bin", varied);
    writer.close();

    //! [THEN] The bomb is refused, the others read back whole
    ZipReader reader(device);
    EXPECT_TRUE(reader.fileData("bomb.bin").empty());
    EXPECT_EQ(reader.fileData("score.mscx"), "<museScore/>");
    EXPECT_EQ(reader.fileData("big.bin"), varied);
    reader.close();
}
