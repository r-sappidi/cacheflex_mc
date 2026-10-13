# VER-04 review (2026-10-11)

15/15 cases accepted. Legacy switch (FINITE_RETURN_LANE=0) reproduces VER-02 v6 n2_s3
exactly (735612/3731573); finite plain n2_s3 passes (739116/3744661) and its instrumented
copy reproduces it exactly. Finite coverage cases pass with no trace or violation:
n2_s2 43467, n2_s3 739116, no-retention n2_s3 723297, n2_s4 7164818, n3_s3 8218655,
n2_s5 48667606/252319802 states/rules. All six mutants caught (silent-registers again
survives n2_s3 and is caught at n3_s3).

Union over retained-LLC finite cases: 250/346 covers (VER-03: 242), 61/67 rules (59).
Newly reached: E/M SPMCP recall rules, L0 WriteBack absorbed in EX_L0/MX_L0 (SLICC F2),
recall IACK completion in EX_L0 (to EE, or MM when dirty) and MX_L0. None lost.
n2_s4 and n2_s5 again reach identical sets (249).

Remaining VER-03 category-3 covers are L0 DataAck handling in SX_L0/EX_L0/MX_L0: dead
under retention because the recall is issued only when L0 no longer holds the line
(L0 answers IACK; dirty data arrives via the overtaken WriteBack). Other VER-03 review
categories are unchanged (legacy source-release path, protocol-impossible, safety
contract, RETAIN_LLC=0-only, baseline MESI branch). Directory back-invalidation of the
LLC remains unexercised (single-line abstraction); state as a limitation.
