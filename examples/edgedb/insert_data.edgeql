INSERT Movie {
  title := "Dune",
  actors := {
    (INSERT Person { name := "Timothée Chalamet" }),
    (INSERT Person { name := "Zendaya" })
  }
};
