Installation
============

``msgspec-x`` may be installed via ``pip`` or ``conda``. Note that Python >= 3.9 is required. The basic install has no required dependencies.

**pip**

.. code-block:: shell

    pip install msgspec-x

**conda**

.. code-block:: shell

    conda install msgspec-x -c conda-forge

.. note::
   Do not install both ``msgspec`` and ``msgspec-x`` in the same environment.


Optional Dependencies
---------------------

Depending on your platform, the base install of ``msgspec-x`` may not support
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

    pip install "msgspec-x[toml]"

**conda**

.. code-block:: shell

    conda install msgspec-x-toml -c conda-forge

YAML
~~~~

The YAML_ protocol requires PyYAML_ on all platforms. You may either install
this dependency manually, or depend on the ``yaml`` extra:

**pip**

.. code-block:: shell

    pip install "msgspec-x[yaml]"

**conda**

.. code-block:: shell

    conda install msgspec-x-yaml -c conda-forge


Installing from GitHub
----------------------

If you want wish to use a feature that hasn't been released yet, you may
install from the `development branch on GitHub
<https://github.com/jcrist/msgspec>`__ using ``pip``:

.. code-block:: shell

    pip install git+https://github.com/jcrist/msgspec.git


.. _YAML: https://yaml.org
.. _TOML: https://toml.io
.. _PyYAML: https://pyyaml.org/
.. _tomli: https://github.com/hukkin/tomli
.. _tomli_w: https://github.com/hukkin/tomli-w
