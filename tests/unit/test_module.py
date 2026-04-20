import sys

import pytest


@pytest.mark.skipif(sys.version_info < (3, 12), reason="3.12+ only")
class TestSubinterpreterGuard:
    # sub-interpreters currently not supported. make sure we correctly report that
    @pytest.mark.skipif(sys.version_info >= (3, 13), reason="3.12 only")
    def test_subinterpreter_import_rejected_312(self):
        interpreters = pytest.importorskip("test.support.interpreters")

        interp = interpreters.create()
        try:
            with pytest.raises(match=".*does not support loading in subinterpreters"):
                interp.run("import msgspec._core")
        finally:
            interp.close()

    @pytest.mark.skipif(sys.version_info < (3, 13), reason="3.13+ only")
    def test_subinterpreter_import_rejected_313(self):
        interpreters = pytest.importorskip("_interpreters")

        interp = interpreters.create()
        try:
            res = interpreters.exec(interp, "import msgspec._core")
            assert "does not support loading in subinterpreters" in res.msg
        finally:
            interpreters.destroy(interp)
