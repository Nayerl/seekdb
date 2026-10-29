# AI COLUMN implementation sources

The AI COLUMN foundation is adapted from the Apache-2.0 licensed implementation by Hututuuuuuuuu in https://github.com/oceanbase/seekdb/pull/2217, original commit b33ea00f7 (feat(ai_column): competition round 3 - AI column). The original authorship and cherry-pick source are retained in Git history.

It is integrated on our AI function and File SQL submission be022b20cb3314e735c027f0663c4f67cc6ad622. Integration preserves the existing File SQL system variable and SQL rewrites and uses pragma once for the added headers. Correctness and platform results are recorded separately; the upstream validation claim is not treated as our own verification.
