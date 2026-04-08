Installation
============

``msgspec-arise`` may be installed via ``pip``. Note that Python >= 3.10
is required. The basic install has no required dependencies.

``msgspec-arise`` is a community fork of ``msgspec``. It provides the same
``import msgspec`` interface — the two packages should not be installed
side-by-side.

**pip**

.. code-block:: shell

    pip install msgspec-arise


Optional Dependencies
---------------------

Depending on your platform, the base install of ``msgspec`` may not support
TOML_ or YAML_ without additional dependencies.

TOML
~~~~

The TOML_ protocol requires:

- Python < 3.11: `tomli`_ and `tomli_w`_ for reading and writing TOML.

- Python >= 3.11: `tomli_w`_ for writing TOML. Reading TOML is done using
  the standard library's `tomllib` and requires no additional dependencies.

You may either install these dependencies manually, or depend on the ``toml``
extra:

**pip**

.. code-block:: shell

    pip install "msgspec-arise[toml]"

YAML
~~~~

The YAML_ protocol requires PyYAML_ on all platforms. You may either install
this dependency manually, or depend on the ``yaml`` extra:

**pip**

.. code-block:: shell

    pip install "msgspec-arise[yaml]"


Installing from GitHub
----------------------

If you want to use a feature that hasn't been released yet, you may
install from the `development branch on GitHub
<https://github.com/Siyet/msgspec-arise>`__ using ``pip``:

.. code-block:: shell

    pip install git+https://github.com/Siyet/msgspec-arise.git


.. _YAML: https://yaml.org
.. _TOML: https://toml.io/en/
.. _PyYAML: https://pyyaml.org/
.. _tomli: https://github.com/hukkin/tomli
.. _tomli_w: https://github.com/hukkin/tomli-w
