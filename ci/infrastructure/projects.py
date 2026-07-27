from ci.settings.settings import PROJECT_NAME, PRAKTIKA_BASE_VENV
from praktika.infrastructure import Components, ImageBuilder, Storage, VPC
from praktika.infrastructure.cloud import CloudInfrastructure


# until published in pip
_PRAKTIKA_PACKAGE_BASE_URL = "https://praktika-artifacts-eu-north-1.s3.amazonaws.com/packages"
_PRAKTIKA_COMPAT_VERSION = "0.1"
_PRAKTIKA_WHL = (
    f"{_PRAKTIKA_PACKAGE_BASE_URL}/{_PRAKTIKA_COMPAT_VERSION}/"
    "praktika-0.0.0-py3-none-any.whl"
)
_PRAKTIKA_CONTROLLER_WHL = (
    f"{_PRAKTIKA_PACKAGE_BASE_URL}/{_PRAKTIKA_COMPAT_VERSION}/"
    "praktika_controller-0.0.0-py3-none-any.whl"
)


def _silk_ci_dependencies_component():
    return {
        "name": "silk-ci-dependencies",
        "platform": "Linux",
        "description": "Install Silk CI toolchains and build dependencies",
        "commands": [
            "export DEBIAN_FRONTEND=noninteractive",
            (
                "wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key "
                "> /etc/apt/trusted.gpg.d/apt.llvm.org.asc"
            ),
            (
                ". /etc/os-release && echo \"deb http://apt.llvm.org/"
                "${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-21 main\" "
                "> /etc/apt/sources.list.d/llvm-21.list"
            ),
            (
                "wget -qO- https://apt.kitware.com/keys/"
                "kitware-archive-latest.asc > /etc/apt/trusted.gpg.d/kitware.asc"
            ),
            (
                ". /etc/os-release && echo \"deb https://apt.kitware.com/ubuntu/ "
                "${VERSION_CODENAME} main\" > /etc/apt/sources.list.d/kitware.list"
            ),
            (
                "DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=60 "
                "install -y software-properties-common"
            ),
            "add-apt-repository -y ppa:ubuntu-toolchain-r/test",
            "apt-get -o DPkg::Lock::Timeout=60 update -q",
            (
                "DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=60 "
                "install -y clang-21 clang-format-21 llvm-21 cmake "
                "libstdc++-13-dev ninja-build ccache gdb libboost-dev "
                "libdouble-conversion-dev libelf-dev zlib1g-dev"
            ),
        ],
    }


def _silk_ci_image_test_component():
    return Components.create_image_test_component(
        name="silk-ci-image-test",
        description="Validate Silk CI toolchains and build dependencies",
        commands=[
            "test -d /opt/praktika/work",
            "test -w /opt/praktika/work",
            "test -x /usr/bin/clang-21",
            "test -x /usr/bin/clang-format-21",
            "test -x /usr/bin/cmake",
            "test -x /usr/bin/ninja",
            "test -x /usr/bin/ccache",
            "test -x /usr/bin/gdb",
            "clang-21 --version",
            "clang-format-21 --version",
            "cmake --version",
            "ninja --version",
            "ccache --version",
            "gdb --version",
            (
                "for package in llvm-21 libstdc++-13-dev gdb libboost-dev "
                "libdouble-conversion-dev libelf-dev zlib1g-dev; do "
                "dpkg-query -W -f='${Status}\\n' \"$package\" "
                "| grep -qx 'install ok installed'; done"
            ),
        ],
    )


def _praktika_controller_image_test_component():
    controller = "praktika-controller"
    start_script = f"/usr/local/bin/${{controller}}-start"
    service_unit = f"/etc/systemd/system/${{controller}}.service"
    return Components.create_image_test_component(
        name="silk-praktika-controller-image-test",
        description="Validate Praktika controller runtime and boot wiring",
        commands=[
            f"controller={controller}; command -v \"$controller\"",
            (
                f"controller={controller}; "
                "python3.12 -m pip show \"$controller\""
            ),
            f"controller={controller}; test -x {start_script}",
            f"controller={controller}; bash -n {start_script}",
            f"controller={controller}; test -f {service_unit}",
            (
                f"controller={controller}; "
                f"grep -qx \"ExecStart={start_script}\" {service_unit}"
            ),
            (
                f"controller={controller}; "
                f"grep -qx \"StandardOutput=append:/var/log/${{controller}}.log\" {service_unit}"
            ),
            (
                f"controller={controller}; "
                f"grep -qx \"StandardError=append:/var/log/${{controller}}.log\" {service_unit}"
            ),
            (
                "test -x /usr/local/bin/praktika-configure-cloudwatch-agent "
                "&& bash -n /usr/local/bin/praktika-configure-cloudwatch-agent"
            ),
            "test -x /opt/aws/amazon-cloudwatch-agent/bin/amazon-cloudwatch-agent-ctl",
        ],
    )


def _silk_ci_image_components():
    return [
        _silk_ci_dependencies_component(),
        _silk_ci_image_test_component(),
        _praktika_controller_image_test_component(),
    ]


def _praktika_launch_user_data():
    return "\n".join(
        [
            "#!/usr/bin/env bash",
            "set -xeuo pipefail",
            "",
            "# Refresh Praktika controller and runtime from the compat channel on launch.",
            f"python3.12 -m pip install --ignore-installed {_PRAKTIKA_CONTROLLER_WHL} --break-system-packages",
            "/usr/local/bin/praktika-configure-cloudwatch-agent",
            "/opt/aws/amazon-cloudwatch-agent/bin/amazon-cloudwatch-agent-ctl -a fetch-config -m ec2 -c file:/etc/praktika/amazon-cloudwatch-agent.json -s",
            (
                f"/opt/praktika/base-venvs/{PRAKTIKA_BASE_VENV}/bin/python "
                f"-m pip install --force-reinstall {_PRAKTIKA_WHL}"
            ),
            "systemctl enable --now praktika-controller",
            "",
        ]
    )


def _image_builders():
    image_recipe_version = "1.0.10"
    prebuilt_venvs = [
        ImageBuilder.PrebuiltVenv(
            name=PRAKTIKA_BASE_VENV,
            packages=[
                "boto3",
                "PyJWT",
                "cryptography",
                "requests",
                "pytest>=7.0.0",
                "pytest-reportlog>=0.4.0",
                _PRAKTIKA_WHL,
            ],
            description=(
                "Shared Python base venv with pytest, Praktika runtime deps, "
                "and Praktika"
            ),
        ),
    ]
    return [
        Components.create_ubuntu_image_builder_config(
            name="ci-arm64-image",
            version=image_recipe_version,
            controller_package=_PRAKTIKA_CONTROLLER_WHL,
            prebuilt_venvs=prebuilt_venvs,
            instance_types=["t4g.small"],
            components=_silk_ci_image_components(),
        ),
        Components.create_ubuntu_image_builder_config(
            name="ci-x86_64-image",
            version=image_recipe_version,
            controller_package=_PRAKTIKA_CONTROLLER_WHL,
            prebuilt_venvs=prebuilt_venvs,
            instance_types=["t3.small"],
            components=_silk_ci_image_components(),
        ),
    ]


_GH_TOKEN_MINTER = Components.GitHubTokenMinter(
    permissions={
        "checks": "write",
        "contents": "write",
        "issues": "write",
        "metadata": "read",
        "pages": "write",
        "pull_requests": "write",
        "statuses": "write",
    },
    repositories=[PROJECT_NAME],
)
_IMAGE_BUILDERS = _image_builders()
_IMAGE_BUILDERS_BY_NAME = {builder.name: builder for builder in _IMAGE_BUILDERS}

PROJECTS = [
    CloudInfrastructure.Config(
        name=PROJECT_NAME,
        min_praktika_version="0.1.4",
        vpcs=[
            VPC.Config(
                subnets=[
                    VPC.Subnet(availability_zone="eu-north-1a"),
                ],
            )
        ],
        storages=[
            Storage.Config(
                name="artifacts-eu-north-1",
                retention_days=30,
                public=True,
            ),
        ],
        report_pages=[Components.report_page_config],
        image_builders=_IMAGE_BUILDERS,
        github_token_minters=[_GH_TOKEN_MINTER],
        orchestrator_pool=Components.OrchestratorPool(
            instance_type="t4g.small",
            scaling=Components.OrchestratorPool.Scaling.Auto,
            size=0,
            max_size=50,
            volume_size_gb=100,
            capacity_reserve=1,
            image_builder=_IMAGE_BUILDERS_BY_NAME["ci-arm64-image"],
            ext={"allowed_push_branches": ["main"]},
            user_data=_praktika_launch_user_data(),
        ),
        runner_pools=[
            Components.RunnerPool(
                name="arm-small",
                instance_type="t4g.medium",
                scaling=Components.RunnerPool.Scaling.Auto,
                size=0,
                max_size=50,
                volume_size_gb=100,
                image_builder=_IMAGE_BUILDERS_BY_NAME["ci-arm64-image"],
                allowed_ssm_parameters=[],
                user_data=_praktika_launch_user_data(),
                allowed_secrets=[],
                allowed_s3_prefixes=["artifacts-eu-north-1"],
                allow_all_ssm_parameters=False,
                allow_all_secrets=False,
                allow_all_s3_prefixes=False,
                allow_ssm_debug=False,
            ),
            Components.RunnerPool(
                name="amd-small",
                instance_type="t3.medium",
                scaling=Components.RunnerPool.Scaling.Auto,
                size=0,
                max_size=50,
                volume_size_gb=100,
                image_builder=_IMAGE_BUILDERS_BY_NAME["ci-x86_64-image"],
                allowed_ssm_parameters=[],
                user_data=_praktika_launch_user_data(),
                allowed_secrets=[],
                allowed_s3_prefixes=["artifacts-eu-north-1"],
                allow_all_ssm_parameters=False,
                allow_all_secrets=False,
                allow_all_s3_prefixes=False,
                allow_ssm_debug=False,
            ),
            Components.RunnerPool(
                name="arm-medium",
                instance_type="c7g.4xlarge",
                scaling=Components.RunnerPool.Scaling.Auto,
                size=0,
                max_size=50,
                volume_size_gb=100,
                image_builder=_IMAGE_BUILDERS_BY_NAME["ci-arm64-image"],
                allowed_ssm_parameters=[],
                user_data=_praktika_launch_user_data(),
                allowed_secrets=[],
                allowed_s3_prefixes=["artifacts-eu-north-1"],
                allow_all_ssm_parameters=False,
                allow_all_secrets=False,
                allow_all_s3_prefixes=False,
                allow_ssm_debug=False,
            ),
            Components.RunnerPool(
                name="amd-medium",
                instance_type="c7a.4xlarge",
                scaling=Components.RunnerPool.Scaling.Auto,
                size=0,
                max_size=50,
                volume_size_gb=100,
                image_builder=_IMAGE_BUILDERS_BY_NAME["ci-x86_64-image"],
                allowed_ssm_parameters=[],
                user_data=_praktika_launch_user_data(),
                allowed_secrets=[],
                allowed_s3_prefixes=["artifacts-eu-north-1"],
                allow_all_ssm_parameters=False,
                allow_all_secrets=False,
                allow_all_s3_prefixes=False,
                allow_ssm_debug=False,
            ),
        ],
    )
]
