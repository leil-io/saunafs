//#include <gmock/gmock.h>
//#include <gtest/gtest.h>
//#include "chunk_trash_manager.h"
//#include "chunk_trash_manager_impl.h"
//
//class MockChunkTrashManagerImpl : public ChunkTrashManagerImpl {
//public:
//	MOCK_METHOD(int, moveToTrash, (const std::filesystem::path&, const std::filesystem::path&, const std::time_t&), (override));
//	MOCK_METHOD(void, init, (const std::string&), (override));
//	MOCK_METHOD(void, collectGarbage, (), (override));
//};
//
//using ::testing::Return;
//using ::testing::Invoke;
//
//class ChunkTrashManagerTest : public ::testing::Test {
//protected:
//	MockChunkTrashManagerImpl mockImpl;
////	ChunkTrashManager trashManager;
//
//	void SetUp() override {
//		// Assuming ChunkTrashManager has a way to set the implementation.
//		ChunkTrashManager::instance().pImpl =
//				std::make_unique<MockChunkTrashManagerImpl>(mockImpl);
//	}
//};
//
//TEST_F(ChunkTrashManagerTest, MoveToTrashForwardsCall) {
//	std::filesystem::path filePath = "example.txt";
//	std::filesystem::path diskPath = "/disk/";
//	std::time_t deletionTime = 1234567890;
//
//	EXPECT_CALL(mockImpl, moveToTrash(filePath, diskPath, deletionTime))
//			.Times(1)
//			.WillOnce(Return(0));  // Assuming 0 is a success return value.
//
//	int result = trashManager.moveToTrash(filePath, diskPath, deletionTime);
//	EXPECT_EQ(result, 0);  // Validate that the return value is as expected.
//}
//
//TEST_F(ChunkTrashManagerTest, InitForwardsCall) {
//	std::string diskPath = "/disk/";
//
//	EXPECT_CALL(mockImpl, init(diskPath)).Times(1);
//
//	trashManager.init(diskPath);  // Call the method.
//}
//
//TEST_F(ChunkTrashManagerTest, CollectGarbageForwardsCall) {
//	EXPECT_CALL(mockImpl, collectGarbage()).Times(1);
//
//	trashManager.collectGarbage();  // Call the method.
//}
