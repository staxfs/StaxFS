#pragma once

namespace dfs {

enum RPCType : int {
  kHelloReq = 1,
  kMetaGeneralReq = 2,
  kMetaPathCommonReq = 3,
  kMetaFDCommonReq = 4,
  kDataReq = 5,

  kMetaCommunicationReq = 6,
  kGuardianCommunicationReq = 7,
  kTestMetaCommunicationReq = 8,
  kGuardianCommonReq = 9,
  kLoadBalanceReq = 10,
  kGetNodeReq = 11,
  kModifyInodeReq = 12,
  kSpecialReq = 13,
  kMetaPersistenceReq = 14,
  kMetaPersistenceReqSplit = 15,
};

} // namespace dfs